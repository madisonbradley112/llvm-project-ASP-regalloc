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

/// Solve \p Program with Clingo and return the best in_gprc/2 assignment.
/// Returns false (no hints) on UNSAT, time-out before any model, or error.
bool runPhase1Clingo(const std::string &Program,
                     std::vector<std::pair<unsigned, unsigned>> &Out) {
  clingo_control_t *Ctl = nullptr;
  char const *Args[] = {"--opt-mode=opt"};
  if (!clingo_control_new(Args, 1, nullptr, nullptr, 20, &Ctl))
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

  std::ostringstream Facts;
  unsigned NextCand = 0;

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

      unsigned CandId = NextCand++;
      ++NumCandidates;
      Facts << "cand(i" << CandId << "). cand_saving(i" << CandId << ", 2).";
      for (Register Reg : VRegs)
        Facts << " needs_gprc(i" << CandId << ", " << getId(Reg) << ").";
      Facts << "\n";
      if (TieA && TieB)
        Facts << "tied(" << getId(TieA) << ", " << getId(TieB) << ").\n";
    }
  }

  if (NextCand == 0)
    return false;

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
  for (unsigned Id = 0, E = IdToVReg.size(); Id < E; ++Id) {
    auto It = AbiPhys.find(IdToVReg[Id]);
    if (It == AbiPhys.end())
      continue;
    int Pool = gprcPoolIndex(It->second);
    if (Pool >= 0)
      Facts << "precolored(" << Id << ", " << Pool << ").\n";
    else
      Facts << "pinned_nongprc(" << Id << ").\n";
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

  // Interference among candidate vregs (emit each pair once, V1 < V2).
  for (unsigned A = 0, N = IdToVReg.size(); A < N; ++A)
    for (unsigned B = A + 1; B < N; ++B)
      if (LIS.getInterval(IdToVReg[A]).overlaps(LIS.getInterval(IdToVReg[B])))
        Facts << "interfere(" << A << ", " << B << ").\n";

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

  // Apply one hint per assigned vreg.  Tied vregs are given the same register
  // by the solver, so coalescing falls out automatically.
  DenseSet<unsigned> Hinted;
  for (auto [VIdx, RIdx] : Assignment) {
    if (VIdx >= IdToVReg.size() || RIdx >= GPRCRegs.size())
      continue;
    if (!Hinted.insert(VIdx).second)
      continue;
    Register VReg = IdToVReg[VIdx];
    MCPhysReg PhysReg = GPRCRegs[RIdx];
    MRI.addRegAllocationHint(VReg, PhysReg);
    ++NumGPRCHints;
    LLVM_DEBUG(dbgs() << "RISCV-ASP-RVC: hint " << printReg(VReg) << " -> "
                      << printReg(PhysReg, ST.getRegisterInfo()) << "\n");
  }
  return false; // hints only; nothing in the function changed.
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
