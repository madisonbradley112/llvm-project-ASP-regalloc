//===- RISCVRVCRegAllocHints.cpp - ASP-driven GPRC allocation hints -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Phase-1 of a two-phase, compression-aware register allocation scheme for the
// RISC-V "C" extension.
//
// The 8 RVC-addressable registers x8-x15 (the GPRC class) are the scarce
// resource: a compressed (16-bit) encoding of a GPRC-requiring instruction is
// only available when all of its register operands live in GPRC.  This pass
// runs just before the register allocator, models "which compression-candidate
// vregs should occupy GPRC so that the most candidates compress" as an Answer
// Set Programming optimization problem, solves it with Clingo, and records the
// resulting exact-colour choices as register-allocation hints
// (MRI.addRegAllocationHint).  The greedy allocator (phase 2) then honours
// those hints where register pressure allows and falls back to ordinary GPR
// registers otherwise -- an instruction that cannot get GPRC simply stays in
// its 32-bit form ("demoted"); no explicit spill machinery is needed.
//
// This is the optimal, global counterpart to the heuristic, per-vreg GPRC
// hinting in RISCVRegisterInfo::getRegAllocationHints().
//
// Modelling notes
// ---------------
// * Two-address ALU ops (c.and/c.or/c.xor/c.sub/c.addw/c.subw and the Zcb
//   unary/c.mul forms, plus c.andi/c.srli/c.srai) compress only when the
//   destination and one source share a register (rd == rs1, or rd == rs2 for a
//   commutable op).  This is expressed with a tied/2 fact: two tied vregs must
//   occupy the same GPRC register or neither is placed.  Because a tied pair
//   that interferes can never share a register, such a candidate is simply
//   left un-realized -- exactly the cases where coalescing would have needed an
//   extra copy.  The tied source is chosen (here, using liveness) to match the
//   one the TwoAddressInstruction pass will pick, i.e. the source that dies at
//   the instruction.
// * Loads/stores (c.lw/c.sw, c.ld/c.sd, and the Zcb byte/half forms) need both
//   their base and data operands in GPRC and a scaled immediate in range; they
//   carry no tie.  These are not hinted by the in-tree heuristic at all, so
//   they are pure additional compression opportunities.
// * Callee-saved GPRC registers (x8/x9) cost a prologue save/restore when not
//   otherwise used.  Rather than a fixed tie-break, the objective subtracts the
//   *real* spill cost of touching them -- a one-time frame surcharge plus a
//   per-register cost -- from the compression bytes saved, all at one priority
//   level.  Those two weights (cs_surcharge/1, cs_per_reg/1) are emitted by the
//   pass according to the active spill mechanism: an inline frame pays a real
//   c.sdsp/c.ldsp pair per register, whereas the save-restore millicode and the
//   Zcmp cm.push/cm.pop instructions amortize that to ~zero marginal cost.  So
//   x10-x15 are preferred under an inline frame, while under Zcmp the pass will
//   freely spend x8/x9 on extra compressions.
// * ABI-pinned operands are modelled explicitly.  A vreg copied directly to/from
//   a physical GPR (argument, return value, call operand) is almost always
//   coalesced into that register, so the pass pins it: precolored/2 fixes a vreg
//   already bound to a GPRC physreg, and pinned_nongprc/1 marks one bound to a
//   non-GPRC physreg (x16/x17/t*, sp, ...) as un-placeable.  Without this the
//   solver would plan placements -- e.g. pulling a 7th pointer argument out of
//   x16 -- that the coalescer simply undoes, producing hints that mislead the
//   greedy allocator into a *worse* result than no hints at all.
//
// Gated behind -riscv-asp-rvc-regalloc (off by default); a no-op when LLVM is
// built without Clingo.
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVSubtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#ifdef LLVM_PBQP_HAVE_CLINGO
#include <atomic>
#include <chrono>
#include <clingo.h>
#include <cstring>
#include <thread>
#endif

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "riscv-asp-rvc"
#define RISCV_ASP_RVC_NAME "RISC-V ASP-driven RVC register allocation hints"

STATISTIC(NumGPRCHints, "Number of GPRC allocation hints emitted by ASP");
STATISTIC(NumCandidates, "Number of GPRC compression candidates seen by ASP");

static cl::opt<bool> EnableASPRVCRegAlloc(
    "riscv-asp-rvc-regalloc", cl::Hidden, cl::init(false),
    cl::desc("Use the ASP/Clingo phase-1 GPRC allocator to emit "
             "compression-aware register-allocation hints"));

// Wall-clock budget for the Clingo solve.  A timed-out solve simply yields no
// hints (the allocator behaves exactly as it would without this pass).
static cl::opt<unsigned> ASPRVCTimeLimitSecs(
    "riscv-asp-rvc-time-limit", cl::Hidden, cl::init(10),
    cl::desc("Clingo time limit (seconds) for the phase-1 GPRC solve"));

// Contention gate: a compression candidate is only modelled/hinted when one of
// its GPRC operands competes with at least this many other candidate operands
// (interference degree).  Below it, GPRC is uncontended and greedy already
// compresses the instruction unaided, so a hint would be inframarginal and only
// perturb the allocator.  Default ~ the GPRC pool size, so the pass intervenes
// only once demand approaches the eight x8-x15 registers.
static cl::opt<unsigned> RVCMinPressure(
    "riscv-asp-rvc-min-pressure", cl::Hidden, cl::init(8),
    cl::desc("Minimum GPRC interference degree for the ASP pass to hint a "
             "compression candidate"));

// Binding channel: when set, the solver's chosen GPRC register for a candidate
// vreg is *bound* by constraining the vreg to that single-register class (the
// allocator must use it or spill), instead of emitting a soft preference the
// greedy allocator may ignore.  The model is call-aware so a bound placement
// never forces a spill around a call.  Set to false to fall back to soft hints.
static cl::opt<bool> RVCBindHints(
    "riscv-asp-rvc-bind", cl::Hidden, cl::init(true),
    cl::desc("Bind the ASP GPRC assignment via register-class constraints "
             "instead of soft allocation hints"));

// Experimental GPRC-occupancy cap (off by default): at any program point place
// at most (GPRC pool size - this) candidates in GPRC.  This proved a poor lever
// -- denying a candidate GPRC does not free a register (it still occupies a
// non-GPRC GPR), so the cap loses compressions without relieving pressure.  The
// register-pressure gate below is the effective mechanism.  Kept as a knob.
static cl::opt<unsigned> RVCHeadroom(
    "riscv-asp-rvc-headroom", cl::Hidden, cl::init(0),
    cl::desc("GPRC registers to leave free for non-candidate values at each "
             "program point (0 = no occupancy cap)"));

// Register-pressure gate: if the function's peak GPR live-range pressure exceeds
// this, it is already register-bound and will spill regardless; forcing values
// into the 8-register GPRC subset there only over-subscribes it and adds spill
// traffic.  Skip such functions entirely (leave them to greedy).  Functions with
// pressure below the bar have spare registers, so forcing candidates into GPRC
// displaces nothing and is a clean compression win.
static cl::opt<unsigned> RVCMaxPressure(
    "riscv-asp-rvc-max-pressure", cl::Hidden, cl::init(26),
    cl::desc("Skip the ASP pass in functions whose peak GPR register pressure "
             "exceeds this (already register-bound; intervention backfires)"));

// Per-candidate spill cost folded into the objective.  Placing a vreg in GPRC
// at a program point where GPR pressure is at/above RVCSpillThreshold restricts
// it to the 8-register GPRC subset where there is no room, which forces a spill
// of some value -- a code-size cost the model must weigh against the 2-byte
// compression.  RVCSpillWeight is that cost in bytes (a store + reload); 0
// disables it.  Unlike the coarse gate, this lets a partially-pressured
// function still compress its low-pressure regions while declining the
// saturated ones.
static cl::opt<unsigned> RVCSpillWeight(
    "riscv-asp-rvc-spill-weight", cl::Hidden, cl::init(0),
    cl::desc("Byte cost charged in the objective for placing a candidate in "
             "GPRC within a register-saturated region (0 = disabled)"));

static cl::opt<unsigned> RVCSpillThreshold(
    "riscv-asp-rvc-spill-threshold", cl::Hidden, cl::init(26),
    cl::desc("GPR pressure at/above which placing a candidate in GPRC is "
             "charged the spill weight"));

namespace {

/// The static policy half of the phase-1 ASP program (mirrors
/// ASP-PBQP-regalloc/regalloc_phase1_gprc.lp).  Dynamic facts -- gprc_reg/1,
/// gprc_callee_saved/1, cand/1, cand_saving/2, needs_gprc/2, tied/2,
/// interfere/2 -- are appended per MachineFunction.
const char *const kPhase1Prelude =
    "% Phase-1 GPRC allocation: globally maximize realized RVC compressions,\n"
    "% then prefer caller-saved GPRC registers.\n"
    "% A vreg is a free GPRC-placement candidate unless ABI-pinned: pinned to a\n"
    "% GPRC physreg (precolored/2, fixed there) or to a non-GPRC physreg\n"
    "% (pinned_nongprc/1, can never be GPRC -> its candidates are blocked).\n"
    "gprc_vreg(V) :- needs_gprc(_, V), not precolored(V, _), "
    "not pinned_nongprc(V).\n"
    "\n"
    "% Symmetric closure of the two-address tie relation.\n"
    "tied_sym(A, B) :- tied(A, B).\n"
    "tied_sym(A, B) :- tied(B, A).\n"
    "\n"
    "% Each candidate vreg occupies at most one GPRC register.\n"
    "{ in_gprc(V, R) : gprc_reg(R) } 1 :- gprc_vreg(V).\n"
    "% ABI-pinned GPRC operands are fixed to their physical register.\n"
    "in_gprc(V, R) :- precolored(V, R).\n"
    "in_gprc(V) :- in_gprc(V, _).\n"
    "\n"
    "% No two simultaneously-live vregs share a GPRC register.\n"
    ":- in_gprc(V1, R), in_gprc(V2, R), interfere(V1, V2).\n"
    "\n"
    "% Two-address operands share one GPRC register (coalesced) or neither is\n"
    "% placed.  With interference, a pair that cannot coalesce for free is\n"
    "% never placed -> its candidate is demoted.\n"
    ":- tied_sym(V1, V2), in_gprc(V1, R), not in_gprc(V2, R).\n"
    "\n"
    "% A vreg that is live across a call cannot occupy a caller-saved GPRC\n"
    "% register (x10-x15 are clobbered by the call); it may only use a\n"
    "% callee-saved GPRC register (x8/x9), which is preserved across the call.\n"
    "% This keeps a *bound* placement from forcing a spill/reload around a call.\n"
    ":- in_gprc(V, R), live_across_call(V), gprc_reg(R), "
    "not gprc_callee_saved(R).\n"
    "\n"
    "% GPRC-occupancy headroom: at each high-pressure program point P, place at\n"
    "% most gprc_cap(K) candidates in GPRC, reserving the rest of x8-x15 for the\n"
    "% values phase-1 does not model (addresses, non-candidates).  point/1 and\n"
    "% at/2 give the candidate vregs live at P.  This stops a bound placement\n"
    "% from monopolizing GPRC and evicting those values to the stack.\n"
    ":- point(P), gprc_cap(K), "
    "C = #count { V : in_gprc(V), at(P, V) }, C > K.\n"
    "\n"
    "% A candidate compresses only if every required operand is in GPRC.\n"
    "blocked(I) :- needs_gprc(I, V), not in_gprc(V).\n"
    "realized(I) :- cand(I), not blocked(I).\n"
    "\n"
    "% Distinct callee-saved GPRC registers touched.  Touching any incurs a\n"
    "% one-time frame surcharge (cs_surcharge/1); each one also costs cs_per_reg/1.\n"
    "% Both weights are supplied by the pass in the same byte unit as\n"
    "% cand_saving and reflect the active spill mechanism: an inline frame pays\n"
    "% a real save/restore per register, whereas save-restore millicode and\n"
    "% Zcmp cm.push/cm.pop amortize that to near zero marginal cost.\n"
    "used_callee_saved(R) :- in_gprc(_, R), gprc_callee_saved(R).\n"
    "any_callee_saved :- used_callee_saved(_).\n"
    "\n"
    "% Objective (single @2 level): maximize realized compression bytes minus\n"
    "% the true spill cost of any callee-saved GPRC registers touched, so a\n"
    "% candidate is realized only when its saving exceeds the spill it forces.\n"
    "#maximize { B@2, realized, I : realized(I), cand_saving(I, B) }.\n"
    "#minimize { S@2, surcharge : any_callee_saved, cs_surcharge(S) }.\n"
    "#minimize { C@2, perreg, R : used_callee_saved(R), cs_per_reg(C) }.\n"
    "% Per-candidate spill cost: placing a vreg in GPRC inside a register-\n"
    "% saturated region forces a spill of some value -- charge it in bytes so\n"
    "% the model only does so when the compression it enables is worth more.\n"
    "#minimize { S@2, spill, V : in_gprc(V), cand_spill_cost(V, S) }.\n"
    "\n"
    "#show in_gprc/2.\n";

class RISCVRVCRegAllocHints : public MachineFunctionPass {
public:
  static char ID;

  RISCVRVCRegAllocHints() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // We only attach hints to MRI; no code or analysis is invalidated.
    AU.setPreservesAll();
    AU.addRequired<LiveIntervalsWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  StringRef getPassName() const override { return RISCV_ASP_RVC_NAME; }
};

/// Shape of a GPRC compression candidate.
enum CandKind {
  CK_None,      ///< not a GPRC-requiring compression candidate.
  CK_LoadStore, ///< {data=op0, base=op1} both GPRC, no tie.
  CK_TwoOp,     ///< {rd=op0, rs1=op1} both GPRC, tie rd~rs1.
  CK_RR3        ///< {rd=op0, rs1=op1, rs2=op2} all GPRC, tie rd~dying source.
};

/// Classify \p MI as a GPRC-requiring compression candidate, applying the same
/// opcode/immediate tests as RISCVRegisterInfo::getRegAllocationHints plus the
/// GPRC loads/stores.  \p Commutable is set for reg-reg ALU ops whose tie may
/// target either source.
CandKind classifyCompressible(const MachineInstr &MI, const RISCVSubtarget &ST,
                              bool &Commutable) {
  Commutable = false;
  auto immInRange = [&](unsigned OpNo, auto Pred) -> bool {
    return MI.getOperand(OpNo).isImm() &&
           Pred(static_cast<int64_t>(MI.getOperand(OpNo).getImm()));
  };

  switch (MI.getOpcode()) {
  default:
    return CK_None;

  // --- GPRC loads (data=rd=op0, base=rs1=op1, off=op2) -------------------
  case RISCV::LW:
    return immInRange(2, [](int64_t I) { return isShiftedUInt<5, 2>(I); })
               ? CK_LoadStore
               : CK_None;
  case RISCV::LD:
    return immInRange(2, [](int64_t I) { return isShiftedUInt<5, 3>(I); })
               ? CK_LoadStore
               : CK_None;
  case RISCV::LBU:
    return ST.hasStdExtZcb() &&
                   immInRange(2, [](int64_t I) { return isUInt<2>(I); })
               ? CK_LoadStore
               : CK_None;
  case RISCV::LH:
  case RISCV::LHU:
    return ST.hasStdExtZcb() &&
                   immInRange(2, [](int64_t I) { return isShiftedUInt<1, 1>(I); })
               ? CK_LoadStore
               : CK_None;

  // --- GPRC stores (data=rs2=op0, base=rs1=op1, off=op2) -----------------
  case RISCV::SW:
    return immInRange(2, [](int64_t I) { return isShiftedUInt<5, 2>(I); })
               ? CK_LoadStore
               : CK_None;
  case RISCV::SD:
    return immInRange(2, [](int64_t I) { return isShiftedUInt<5, 3>(I); })
               ? CK_LoadStore
               : CK_None;
  case RISCV::SB:
    return ST.hasStdExtZcb() &&
                   immInRange(2, [](int64_t I) { return isUInt<2>(I); })
               ? CK_LoadStore
               : CK_None;
  case RISCV::SH:
    return ST.hasStdExtZcb() &&
                   immInRange(2, [](int64_t I) { return isShiftedUInt<1, 1>(I); })
               ? CK_LoadStore
               : CK_None;

  // --- reg-reg ALU: rd, rs1, rs2 all GPRC, two-address -------------------
  case RISCV::AND:
  case RISCV::OR:
  case RISCV::XOR:
  case RISCV::ADDW:
    Commutable = true;
    return CK_RR3;
  case RISCV::SUB:
  case RISCV::SUBW:
    return CK_RR3;
  case RISCV::MUL: // c.mul (Zcb)
    if (!ST.hasStdExtZcb())
      return CK_None;
    Commutable = true;
    return CK_RR3;

  // --- reg-imm / unary ALU: rd, rs1 GPRC, tie rd~rs1 ---------------------
  case RISCV::ANDI:
    if (immInRange(2, [](int64_t I) { return isInt<6>(I); }))
      return CK_TwoOp;
    // c.zext.b
    if (ST.hasStdExtZcb() && immInRange(2, [](int64_t I) { return I == 255; }))
      return CK_TwoOp;
    return CK_None;
  case RISCV::SRAI:
  case RISCV::SRLI:
    return CK_TwoOp;
  case RISCV::SEXT_B:
  case RISCV::SEXT_H:
  case RISCV::ZEXT_H_RV32:
  case RISCV::ZEXT_H_RV64:
    return ST.hasStdExtZcb() ? CK_TwoOp : CK_None; // c.sext.*/c.zext.h
  case RISCV::ADD_UW:                              // c.zext.w
    return ST.hasStdExtZcb() && MI.getOperand(2).isReg() &&
                   MI.getOperand(2).getReg() == RISCV::X0
               ? CK_TwoOp
               : CK_None;
  case RISCV::XORI: // c.not
    return ST.hasStdExtZcb() &&
                   immInRange(2, [](int64_t I) { return I == -1; })
               ? CK_TwoOp
               : CK_None;
  }
}

#ifdef LLVM_PBQP_HAVE_CLINGO

/// Extract (V, R) from an `in_gprc(V, R)` Clingo symbol.
bool parseInGprcAtom(clingo_symbol_t Sym, unsigned &V, unsigned &R) {
  if (clingo_symbol_type(Sym) != clingo_symbol_type_function)
    return false;
  char const *Name = nullptr;
  if (!clingo_symbol_name(Sym, &Name) || std::strcmp(Name, "in_gprc") != 0)
    return false;
  clingo_symbol_t const *Args = nullptr;
  size_t NumArgs = 0;
  if (!clingo_symbol_arguments(Sym, &Args, &NumArgs) || NumArgs != 2)
    return false;
  int VN = 0, RN = 0;
  if (!clingo_symbol_number(Args[0], &VN) || !clingo_symbol_number(Args[1], &RN))
    return false;
  V = static_cast<unsigned>(VN);
  R = static_cast<unsigned>(RN);
  return true;
}

/// Drop Clingo's grounder/solver diagnostics (e.g. "atom does not occur in any
/// rule head" for predicates a given function emits no facts for) instead of
/// letting them leak onto the compiler's stderr.
void clingoSilentLogger(clingo_warning_t, char const *, void *) {}

/// Solve \p Program with Clingo and return the best in_gprc/2 assignment.
/// Returns false (no hints) on UNSAT, time-out before any model, or error.
bool runPhase1Clingo(const std::string &Program,
                     std::vector<std::pair<unsigned, unsigned>> &Out) {
  clingo_control_t *Ctl = nullptr;
  char const *Args[] = {"--opt-mode=opt"};
  if (!clingo_control_new(Args, 1, clingoSilentLogger, nullptr, 20, &Ctl))
    return false;

  auto Cleanup = [&]() { clingo_control_free(Ctl); };

  if (!clingo_control_add(Ctl, "base", nullptr, 0, Program.c_str())) {
    Cleanup();
    return false;
  }
  clingo_part_t Parts[] = {{"base", nullptr, 0}};
  if (!clingo_control_ground(Ctl, Parts, 1, nullptr, nullptr)) {
    Cleanup();
    return false;
  }

  clingo_solve_handle_t *Handle = nullptr;
  if (!clingo_control_solve(Ctl, clingo_solve_mode_yield, nullptr, 0, nullptr,
                            nullptr, &Handle)) {
    Cleanup();
    return false;
  }

  // Interrupt the solve after the wall-clock budget; clingo_control_interrupt
  // is the thread-safe SIGINT equivalent.
  std::atomic<bool> Done{false};
  std::thread Timeout([&]() {
    auto Deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(ASPRVCTimeLimitSecs);
    while (!Done.load(std::memory_order_relaxed)) {
      if (std::chrono::steady_clock::now() >= Deadline) {
        clingo_control_interrupt(Ctl);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });

  std::vector<std::pair<unsigned, unsigned>> Latest;
  bool FoundAny = false;
  while (true) {
    if (!clingo_solve_handle_resume(Handle))
      break;
    clingo_model_t const *Model = nullptr;
    if (!clingo_solve_handle_model(Handle, &Model) || !Model)
      break;
    size_t NumAtoms = 0;
    if (!clingo_model_symbols_size(Model, clingo_show_type_shown, &NumAtoms))
      continue;
    std::vector<clingo_symbol_t> Atoms(NumAtoms);
    if (!clingo_model_symbols(Model, clingo_show_type_shown, Atoms.data(),
                              NumAtoms))
      continue;
    std::vector<std::pair<unsigned, unsigned>> Current;
    for (auto Sym : Atoms) {
      unsigned V = 0, R = 0;
      if (parseInGprcAtom(Sym, V, R))
        Current.emplace_back(V, R);
    }
    Latest = std::move(Current);
    FoundAny = true;
  }

  Done.store(true, std::memory_order_relaxed);
  Timeout.join();

  clingo_solve_handle_close(Handle);
  Cleanup();

  if (!FoundAny)
    return false;
  Out = std::move(Latest);
  return true;
}

#endif // LLVM_PBQP_HAVE_CLINGO

} // end anonymous namespace

char RISCVRVCRegAllocHints::ID = 0;

INITIALIZE_PASS_BEGIN(RISCVRVCRegAllocHints, DEBUG_TYPE, RISCV_ASP_RVC_NAME,
                      false, false)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_END(RISCVRVCRegAllocHints, DEBUG_TYPE, RISCV_ASP_RVC_NAME, false,
                    false)

FunctionPass *llvm::createRISCVRVCRegAllocHintsPass() {
  return new RISCVRVCRegAllocHints();
}

bool RISCVRVCRegAllocHints::runOnMachineFunction(MachineFunction &MF) {
  if (!EnableASPRVCRegAlloc || skipFunction(MF.getFunction()))
    return false;

  const RISCVSubtarget &ST = MF.getSubtarget<RISCVSubtarget>();
  if (!ST.hasStdExtZca())
    return false;

  MachineRegisterInfo &MRI = MF.getRegInfo();
  LiveIntervals &LIS = getAnalysis<LiveIntervalsWrapperPass>().getLIS();

  // Dense id for each candidate vreg; the id is what appears in ASP facts.
  DenseMap<Register, unsigned> VRegId;
  std::vector<Register> IdToVReg;
  auto getId = [&](Register R) -> unsigned {
    auto It = VRegId.find(R);
    if (It != VRegId.end())
      return It->second;
    unsigned Id = IdToVReg.size();
    VRegId[R] = Id;
    IdToVReg.push_back(R);
    return Id;
  };

  // A vreg operand is usable iff it has a live interval and a register class
  // that admits GPRC (can hold x10).  A GPRC physreg operand is already
  // satisfied; any other physreg makes the instruction un-compressible.
  enum OpState { OS_Vreg, OS_FixedGPRC, OS_Reject };
  auto classifyOp = [&](const MachineOperand &MO) -> OpState {
    if (!MO.isReg())
      return OS_Reject;
    Register Reg = MO.getReg();
    if (Reg.isPhysical())
      return RISCV::GPRCRegClass.contains(Reg) ? OS_FixedGPRC : OS_Reject;
    if (!LIS.hasInterval(Reg) || !MRI.getRegClass(Reg)->contains(RISCV::X10))
      return OS_Reject;
    return OS_Vreg;
  };

  auto overlaps = [&](Register A, Register B) -> bool {
    return LIS.getInterval(A).overlaps(LIS.getInterval(B));
  };

  // ABI affinity: a virtual register copied directly to or from a physical GPR
  // is almost always coalesced into that physical register (function arguments,
  // return values, call operands).  Treat such a vreg as pinned to that physreg
  // so the model never plans a GPRC placement the coalescer will simply undo --
  // e.g. trying to pull a 7th pointer argument (a6/x16) into GPRC.  A vreg with
  // conflicting affinities (copied to two different physregs) is left unpinned.
  DenseMap<Register, Register> AbiPhys;
  DenseSet<Register> AbiAmbiguous;
  auto noteAbi = [&](Register VReg, Register Phys) {
    if (AbiAmbiguous.count(VReg))
      return;
    auto It = AbiPhys.find(VReg);
    if (It == AbiPhys.end())
      AbiPhys[VReg] = Phys;
    else if (It->second != Phys) {
      AbiAmbiguous.insert(VReg);
      AbiPhys.erase(It);
    }
  };
  for (const MachineBasicBlock &MBB : MF)
    for (const MachineInstr &MI : MBB) {
      if (!MI.isCopy())
        continue;
      Register Dst = MI.getOperand(0).getReg();
      Register Src = MI.getOperand(1).getReg();
      if (Dst.isVirtual() && Src.isPhysical() &&
          RISCV::GPRRegClass.contains(Src))
        noteAbi(Dst, Src);
      else if (Src.isVirtual() && Dst.isPhysical() &&
               RISCV::GPRRegClass.contains(Dst))
        noteAbi(Src, Dst);
    }

  // ---- Collect candidates (facts emitted later, after contention filtering).
  struct Cand {
    SmallVector<unsigned, 3> Needs; // dense ids of operands that need GPRC
    int TieA = -1, TieB = -1;       // two-address tie pair, if any
  };
  std::vector<Cand> Cands;

  for (const MachineBasicBlock &MBB : MF) {
    for (const MachineInstr &MI : MBB) {
      bool Commutable = false;
      CandKind Kind = classifyCompressible(MI, ST, Commutable);
      if (Kind == CK_None)
        continue;

      // Operand indices that must be in GPRC for this candidate.
      SmallVector<unsigned, 3> OpIdxs;
      if (Kind == CK_RR3)
        OpIdxs = {0, 1, 2};
      else
        OpIdxs = {0, 1}; // CK_LoadStore and CK_TwoOp.

      // Collect the virtual-register operands that need GPRC; reject the whole
      // candidate if any required operand can never be GPRC.
      SmallVector<Register, 3> VRegs;     // virtual operands needing GPRC
      bool Ok = true;
      for (unsigned Idx : OpIdxs) {
        switch (classifyOp(MI.getOperand(Idx))) {
        case OS_Reject:
          Ok = false;
          break;
        case OS_FixedGPRC:
          continue; // already GPRC; not an allocation choice.
        case OS_Vreg:
          VRegs.push_back(MI.getOperand(Idx).getReg());
          break;
        }
        if (!Ok)
          break;
      }
      if (!Ok || VRegs.empty())
        continue;

      // Determine the tie (two-address) pair, if any.  CK_TwoOp ties rd(0) to
      // rs1(1).  CK_RR3 ties rd(0) to whichever source dies here (matching the
      // commutation the TwoAddressInstruction pass will choose); if neither
      // source dies the pair cannot coalesce for free and the resulting
      // tied+interfering facts leave the candidate un-realized.
      Register TieA, TieB;
      if (Kind == CK_TwoOp || Kind == CK_RR3) {
        Register Rd = MI.getOperand(0).getReg();
        Register Rs1 = MI.getOperand(1).getReg();
        if (Rd.isVirtual()) {
          if (Kind == CK_TwoOp) {
            if (Rs1.isVirtual()) {
              TieA = Rd;
              TieB = Rs1;
            }
          } else {
            Register Rs2 = MI.getOperand(2).getReg();
            Register Partner;
            if (Rs1.isVirtual() && !overlaps(Rd, Rs1))
              Partner = Rs1;
            else if (Commutable && Rs2.isVirtual() && !overlaps(Rd, Rs2))
              Partner = Rs2;
            else if (Rs1.isVirtual())
              Partner = Rs1; // will be blocked via interference (no free tie).
            if (Partner) {
              TieA = Rd;
              TieB = Partner;
            }
          }
        }
      }

      Cand C;
      for (Register Reg : VRegs)
        C.Needs.push_back(getId(Reg));
      if (TieA && TieB) {
        C.TieA = (int)getId(TieA);
        C.TieB = (int)getId(TieB);
      }
      Cands.push_back(std::move(C));
      ++NumCandidates;
    }
  }

  if (Cands.empty())
    return false;

  // ---- GPR register-pressure model ----------------------------------------
  // The set of GPR-class virtual registers, and a helper giving the number of
  // them live at a slot (the GPR register pressure at that program point).
  // This drives two things: a coarse function-level gate, and the per-candidate
  // spill cost folded into the ASP objective below.
  SmallVector<Register, 256> GPRVRegs;
  for (unsigned I = 0, E = MRI.getNumVirtRegs(); I != E; ++I) {
    Register R = Register::index2VirtReg(I);
    if (LIS.hasInterval(R) && MRI.getRegClass(R)->contains(RISCV::X10))
      GPRVRegs.push_back(R);
  }
  auto pressureAt = [&](SlotIndex S) -> unsigned {
    unsigned N = 0;
    for (Register R : GPRVRegs)
      if (LIS.getInterval(R).liveAt(S))
        ++N;
    return N;
  };
  // Max GPR pressure over a vreg's live range (sampled at segment starts, where
  // pressure peaks since a new value is added there).
  auto maxPressureOver = [&](Register V) -> unsigned {
    unsigned M = 0;
    for (const auto &Seg : LIS.getInterval(V))
      M = std::max(M, pressureAt(Seg.start));
    return M;
  };

  // Coarse gate: peak GPR pressure (max simultaneously-live GPR vregs).  If it
  // exceeds the bar, the whole function is register-bound; skip it (also saves
  // solver time).  The per-candidate spill cost handles partially-pressured
  // functions that pass this gate.
  {
    SmallVector<std::pair<SlotIndex, int>, 256> Ev;
    for (Register R : GPRVRegs)
      for (const auto &Seg : LIS.getInterval(R)) {
        Ev.emplace_back(Seg.start, +1);
        Ev.emplace_back(Seg.end, -1);
      }
    std::sort(Ev.begin(), Ev.end(),
              [](const std::pair<SlotIndex, int> &A,
                 const std::pair<SlotIndex, int> &B) {
                if (A.first != B.first)
                  return A.first < B.first;
                return A.second > B.second; // +1 before -1 at the same slot
              });
    int Cur = 0, Peak = 0;
    for (auto &E : Ev) {
      Cur += E.second;
      Peak = std::max(Peak, Cur);
    }
    if ((unsigned)Peak > RVCMaxPressure) {
      LLVM_DEBUG(dbgs() << "RISCV-ASP-RVC: " << MF.getName() << " peak GPR "
                        << "pressure " << Peak << " > " << RVCMaxPressure
                        << "; skipping (register-bound)\n");
      return false;
    }
  }

  // ---- Interference + per-vreg contention (interference degree) -----------
  const unsigned NumVRegs = IdToVReg.size();
  std::vector<std::pair<unsigned, unsigned>> Interf;
  std::vector<unsigned> Degree(NumVRegs, 0);
  for (unsigned A = 0; A < NumVRegs; ++A)
    for (unsigned B = A + 1; B < NumVRegs; ++B)
      if (overlaps(IdToVReg[A], IdToVReg[B])) {
        Interf.emplace_back(A, B);
        ++Degree[A];
        ++Degree[B];
      }

  // ---- Selective + pressure-aware gate ------------------------------------
  // Only intervene where GPRC is actually contended: keep a candidate only if
  // one of its required operands competes with at least RVCMinPressure other
  // candidate operands (its interference degree).  In an uncontended region
  // greedy already places the value in GPRC on its own, so a hint there is
  // inframarginal -- it cannot add a compression, only perturb the allocator
  // into spilling or rematerializing values (addresses) it would otherwise
  // have kept in registers.  This both suppresses redundant hints and caps how
  // much of the scarce GPRC pool the model may monopolize.
  std::vector<bool> KeepCand(Cands.size(), false);
  std::vector<bool> KeepVReg(NumVRegs, false);
  unsigned NumKept = 0;
  for (unsigned CI = 0; CI < Cands.size(); ++CI) {
    bool Contended = false;
    for (unsigned Id : Cands[CI].Needs)
      if (Degree[Id] >= RVCMinPressure) {
        Contended = true;
        break;
      }
    if (!Contended)
      continue;
    KeepCand[CI] = true;
    ++NumKept;
    for (unsigned Id : Cands[CI].Needs)
      KeepVReg[Id] = true;
  }
  if (NumKept == 0)
    return false; // nothing contended; leave everything to greedy.

  // ---- Emit candidate facts (kept candidates only) ------------------------
  std::ostringstream Facts;
  unsigned CandId = 0;
  for (unsigned CI = 0; CI < Cands.size(); ++CI) {
    if (!KeepCand[CI])
      continue;
    Facts << "cand(i" << CandId << "). cand_saving(i" << CandId << ", 2).";
    for (unsigned Id : Cands[CI].Needs)
      Facts << " needs_gprc(i" << CandId << ", " << Id << ").";
    Facts << "\n";
    if (Cands[CI].TieA >= 0)
      Facts << "tied(" << Cands[CI].TieA << ", " << Cands[CI].TieB << ").\n";
    ++CandId;
  }

  // GPRC physical-register pool: index i -> GPRCRegs[i].  Mark the
  // callee-saved members (x8/x9 under the standard ABI) for the tie-breaker.
  std::vector<MCPhysReg> GPRCRegs(RISCV::GPRCRegClass.begin(),
                                  RISCV::GPRCRegClass.end());
  const MCPhysReg *CSR = MRI.getCalleeSavedRegs();
  auto isCalleeSaved = [&](MCPhysReg Reg) {
    for (const MCPhysReg *P = CSR; P && *P; ++P)
      if (*P == Reg)
        return true;
    return false;
  };
  for (unsigned I = 0, E = GPRCRegs.size(); I != E; ++I) {
    Facts << "gprc_reg(" << I << ").\n";
    if (isCalleeSaved(GPRCRegs[I]))
      Facts << "gprc_callee_saved(" << I << ").\n";
  }

  // ABI-pinned candidate operands.  A vreg pinned to a GPRC physreg is fixed
  // there (precolored/2); one pinned to a non-GPRC physreg can never compress
  // its candidates (pinned_nongprc/1).  This keeps the solver from planning
  // placements the register coalescer will override.
  auto gprcPoolIndex = [&](Register Phys) -> int {
    for (unsigned I = 0, N = GPRCRegs.size(); I != N; ++I)
      if (Register(GPRCRegs[I]) == Phys)
        return (int)I;
    return -1;
  };
  DenseSet<unsigned> PrecoloredIds;
  for (unsigned Id = 0; Id < NumVRegs; ++Id) {
    if (!KeepVReg[Id])
      continue;
    auto It = AbiPhys.find(IdToVReg[Id]);
    if (It == AbiPhys.end())
      continue;
    int Pool = gprcPoolIndex(It->second);
    if (Pool >= 0) {
      Facts << "precolored(" << Id << ", " << Pool << ").\n";
      PrecoloredIds.insert(Id);
    } else
      Facts << "pinned_nongprc(" << Id << ").\n";
  }

  // Per-candidate spill cost: a vreg whose live range crosses a region where
  // GPR pressure is at/above the threshold cannot be forced into the 8-register
  // GPRC subset without evicting some value, so charge its placement the spill
  // weight (in bytes) in the objective.  Precolored vregs already sit in their
  // physical register, so binding them induces nothing -- skip those.
  if (RVCSpillWeight > 0) {
    for (unsigned Id = 0; Id < NumVRegs; ++Id) {
      if (!KeepVReg[Id] || PrecoloredIds.count(Id))
        continue;
      if (maxPressureOver(IdToVReg[Id]) >= RVCSpillThreshold)
        Facts << "cand_spill_cost(" << Id << ", " << RVCSpillWeight << ").\n";
    }
  }

  // Spill-cost weights for callee-saved GPRC use, in bytes (same unit as
  // cand_saving = 2).  Surcharge is paid once if any callee-saved register is
  // touched; per-reg is paid for each.  The values track the active spill
  // mechanism's *marginal* code-size cost:
  //   * Zcmp (cm.push/cm.pop): one tiny pushed/popped list covers the whole
  //     callee-saved range, so adding a register is essentially free.
  //   * save-restore millicode: a fixed call/tail into shared routines, with
  //     ~zero marginal cost per extra register saved by that routine.
  //   * inline frame (default): real c.addi16sp frame setup plus a
  //     c.sdsp/c.ldsp pair per register.
  unsigned CsSurcharge, CsPerReg;
  if (ST.hasStdExtZcmp()) {
    CsSurcharge = 2;
    CsPerReg = 0;
  } else if (ST.enableSaveRestore()) {
    CsSurcharge = 8;
    CsPerReg = 0;
  } else {
    CsSurcharge = 4;
    CsPerReg = 4;
  }
  Facts << "cs_surcharge(" << CsSurcharge << ").\n";
  Facts << "cs_per_reg(" << CsPerReg << ").\n";

  // Interference among kept candidate vregs (reuse the precomputed pairs).
  for (auto [A, B] : Interf)
    if (KeepVReg[A] && KeepVReg[B])
      Facts << "interfere(" << A << ", " << B << ").\n";

  // Vregs live across a call.  Binding such a vreg to a caller-saved GPRC reg
  // (x10-x15, clobbered by the call) would force a spill/reload around it, so
  // the model restricts them to callee-saved GPRC (x8/x9).  Conservatively mark
  // a kept vreg as live-across if its value is still live at the register slot
  // of any call.  Only relevant when binding (soft hints never force a spill).
  if (RVCBindHints) {
    SmallVector<SlotIndex, 8> CallSlots;
    for (const MachineBasicBlock &MBB : MF)
      for (const MachineInstr &MI : MBB)
        if (MI.isCall())
          CallSlots.push_back(LIS.getInstructionIndex(MI).getRegSlot());
    for (unsigned Id = 0; Id < NumVRegs; ++Id) {
      if (!KeepVReg[Id])
        continue;
      const LiveInterval &LI = LIS.getInterval(IdToVReg[Id]);
      for (SlotIndex SI : CallSlots)
        if (LI.liveAt(SI)) {
          Facts << "live_across_call(" << Id << ").\n";
          break;
        }
    }
  }

  // GPRC-occupancy headroom.  Cap how many candidates may sit in GPRC at any one
  // program point, reserving (RVCHeadroom) of the eight registers for values the
  // model cannot see.  Pressure peaks at live-range starts, so snapshot the set
  // of kept candidates live at each candidate segment start and emit a per-point
  // cap for the distinct sets larger than the cap.  RVCHeadroom == 0 (or >= pool
  // size) disables this (interference already bounds occupancy at the pool size).
  unsigned Cap = RVCHeadroom < GPRCRegs.size() ? GPRCRegs.size() - RVCHeadroom : 0;
  if (RVCHeadroom > 0 && Cap < GPRCRegs.size()) {
    Facts << "gprc_cap(" << Cap << ").\n";
    SmallVector<unsigned, 64> Kept;
    for (unsigned Id = 0; Id < NumVRegs; ++Id)
      if (KeepVReg[Id])
        Kept.push_back(Id);
    SmallVector<SlotIndex, 128> Points;
    for (unsigned Id : Kept)
      for (const auto &Seg : LIS.getInterval(IdToVReg[Id]))
        Points.push_back(Seg.start);
    std::set<std::vector<unsigned>> Emitted;
    unsigned PointId = 0;
    for (SlotIndex SI : Points) {
      std::vector<unsigned> S;
      for (unsigned Id : Kept)
        if (LIS.getInterval(IdToVReg[Id]).liveAt(SI))
          S.push_back(Id);
      if (S.size() <= Cap)
        continue; // point not over capacity: no constraint needed.
      if (!Emitted.insert(S).second)
        continue; // identical live-set already constrained.
      unsigned P = PointId++;
      Facts << "point(" << P << ").";
      for (unsigned Id : S)
        Facts << " at(" << P << ", " << Id << ").";
      Facts << "\n";
    }
  }

  std::string Program = std::string(kPhase1Prelude) + Facts.str();
  LLVM_DEBUG(dbgs() << "RISCV-ASP-RVC program for " << MF.getName() << ":\n"
                    << Program << "\n");

#ifdef LLVM_PBQP_HAVE_CLINGO
  std::vector<std::pair<unsigned, unsigned>> Assignment;
  if (!runPhase1Clingo(Program, Assignment)) {
    LLVM_DEBUG(dbgs() << "RISCV-ASP-RVC: no assignment (UNSAT/timeout); "
                         "leaving allocation to greedy\n");
    return false;
  }

  // Realize the assignment, one action per assigned vreg.  Tied vregs share a
  // register in the model, so coalescing/co-binding falls out automatically.
  //
  // Binding constrains the vreg to the *GPRC class* (x8-x15), not to the single
  // register the solver picked.  This still forces the value into the
  // compressible register file -- guaranteeing the candidate compresses -- but
  // leaves the allocator 8-way freedom to satisfy the many fixed-register uses
  // the phase-1 model does not see (ABI arguments, return value, call setup,
  // specific-register instructions).  A single-register constraint instead
  // collides with those fixed uses and makes the allocator run out of
  // registers; binding to the class is crash-safe (worst case it spills).  We
  // still emit the solver's specific register as a soft hint so the allocator
  // prefers the globally-chosen packing within GPRC.
  bool Changed = false;
  DenseSet<unsigned> Applied;
  for (auto [VIdx, RIdx] : Assignment) {
    if (VIdx >= IdToVReg.size() || RIdx >= GPRCRegs.size())
      continue;
    // ABI-pinned vregs already sit in their physical register; touching them is
    // redundant and only adds noise.
    if (PrecoloredIds.count(VIdx))
      continue;
    if (!Applied.insert(VIdx).second)
      continue;
    Register VReg = IdToVReg[VIdx];
    MCPhysReg PhysReg = GPRCRegs[RIdx];

    if (RVCBindHints && MRI.constrainRegClass(VReg, &RISCV::GPRCRegClass)) {
      MRI.addRegAllocationHint(VReg, PhysReg); // prefer the packed register
      ++NumGPRCHints;
      Changed = true;
      LLVM_DEBUG(dbgs() << "RISCV-ASP-RVC: bind " << printReg(VReg)
                        << " -> GPRC (hint " << printReg(PhysReg, ST.getRegisterInfo())
                        << ")\n");
      continue;
    }
    MRI.addRegAllocationHint(VReg, PhysReg);
    ++NumGPRCHints;
    LLVM_DEBUG(dbgs() << "RISCV-ASP-RVC: hint " << printReg(VReg) << " -> "
                      << printReg(PhysReg, ST.getRegisterInfo()) << "\n");
  }
  return Changed;
#else
  static bool Warned = false;
  if (!Warned) {
    Warned = true;
    errs() << "warning: -riscv-asp-rvc-regalloc requires an LLVM build with "
              "Clingo; pass is a no-op.\n";
  }
  return false;
#endif
}
