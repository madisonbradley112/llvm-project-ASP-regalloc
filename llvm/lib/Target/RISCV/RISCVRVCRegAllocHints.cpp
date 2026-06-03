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
// resource: a compressed (16-bit) encoding of a reg-reg instruction is only
// available when its operands live in GPRC.  This pass runs just before the
// register allocator, models "which compression-candidate vregs should occupy
// GPRC, so that the most candidates can be compressed" as an Answer Set
// Programming optimization problem, solves it with Clingo, and records the
// resulting exact-colour choices as register-allocation hints
// (MRI.addRegAllocationHint).  The greedy allocator (phase 2) then honours
// those hints where register pressure allows and falls back to ordinary GPR
// registers otherwise -- i.e. an instruction that cannot get GPRC simply stays
// in its 32-bit form ("demoted"); no explicit spill machinery is needed.
//
// This is the optimal counterpart to the heuristic GPRC hinting already done
// in RISCVRegisterInfo::getRegAllocationHints().  It is gated behind
// -riscv-asp-rvc-regalloc (off by default) and is a no-op when LLVM is built
// without Clingo.
//
// Scope (first cut): GPRC-requiring reg-reg ALU instructions (AND/OR/XOR/SUB,
// ADDW/SUBW, SRLI/SRAI, ANDI).  GPRC loads/stores (c.lw/c.sw/...) and the FP
// compressed classes are deliberately left out for now -- they need immediate
// range checks and a separate FP-compressed pool -- and are a localized
// extension to collectGPRCOperands() / the register pool below.
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVSubtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
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

/// The static policy half of the phase-1 ASP program (mirrors the core of
/// ASP-PBQP-regalloc/regalloc_phase1_gprc.lp).  Dynamic facts -- gprc_reg/1,
/// cand/1, cand_saving/2, needs_gprc/2, interfere/2 -- are appended per
/// MachineFunction.
const char *const kPhase1Prelude =
    "% Phase-1 GPRC allocation: maximize realized RVC compressions.\n"
    "gprc_vreg(V) :- needs_gprc(_, V).\n"
    "{ in_gprc(V, R) : gprc_reg(R) } 1 :- gprc_vreg(V).\n"
    "in_gprc(V) :- in_gprc(V, _).\n"
    "% No two simultaneously-live vregs in the same GPRC register.\n"
    ":- in_gprc(V1, R), in_gprc(V2, R), interfere(V1, V2).\n"
    "% A candidate compresses only if all its required operands got GPRC.\n"
    "blocked(I) :- needs_gprc(I, V), not in_gprc(V).\n"
    "realized(I) :- cand(I), not blocked(I).\n"
    "#maximize { B@1, I : realized(I), cand_saving(I, B) }.\n"
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

/// Return the operand indices of \p MI that must be allocated to GPRC for the
/// instruction to have a compressed (16-bit) encoding, or an empty vector if
/// MI is not a GPRC-requiring compression candidate.
///
/// Mirrors the NeedGPRC cases of RISCVRegisterInfo::getRegAllocationHints'
/// isCompressible helper.  All listed forms are two-address ALU ops, so their
/// register operands are GPR-class -- every GPRC register is a legal colour and
/// no per-vreg allowed/2 filtering is required.
SmallVector<unsigned, 3> collectGPRCOperands(const MachineInstr &MI,
                                             const RISCVSubtarget &ST) {
  switch (MI.getOpcode()) {
  default:
    return {};
  case RISCV::AND:
  case RISCV::OR:
  case RISCV::XOR:
  case RISCV::SUB:
  case RISCV::ADDW:
  case RISCV::SUBW:
    // c.and / c.or / c.xor / c.sub / c.addw / c.subw : rd, rs1, rs2 all GPRC.
    return {0, 1, 2};
  case RISCV::SRAI:
  case RISCV::SRLI:
    // c.srai / c.srli : rd, rs1 GPRC (shift amount is an immediate).
    return {0, 1};
  case RISCV::ANDI: {
    // c.andi : rd, rs1 GPRC, with a sign-extended 6-bit immediate (or the
    // Zcb c.zext.b form, imm == 255).
    if (!MI.getOperand(2).isImm())
      return {};
    int64_t Imm = MI.getOperand(2).getImm();
    if (isInt<6>(Imm) || (ST.hasStdExtZcb() && Imm == 255))
      return {0, 1};
    return {};
  }
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

  std::ostringstream Facts;
  unsigned NextCand = 0;
  bool AnyCandidate = false;

  for (const MachineBasicBlock &MBB : MF) {
    for (const MachineInstr &MI : MBB) {
      SmallVector<unsigned, 3> OpIdxs = collectGPRCOperands(MI, ST);
      if (OpIdxs.empty())
        continue;

      // Gather the virtual-register operands that must be GPRC. If any required
      // operand is a physical register outside GPRC, the instruction can never
      // compress -- skip it entirely.
      SmallVector<Register, 3> Needs;
      bool Compressible = true;
      for (unsigned Idx : OpIdxs) {
        const MachineOperand &MO = MI.getOperand(Idx);
        if (!MO.isReg()) {
          Compressible = false;
          break;
        }
        Register Reg = MO.getReg();
        if (Reg.isPhysical()) {
          if (!RISCV::GPRCRegClass.contains(Reg)) {
            Compressible = false;
            break;
          }
          continue; // already a fixed GPRC physreg; not an allocation choice.
        }
        // Virtual: must have a live interval and a class that admits GPRC.
        if (!LIS.hasInterval(Reg) ||
            !MRI.getRegClass(Reg)->contains(RISCV::X10)) {
          Compressible = false;
          break;
        }
        Needs.push_back(Reg);
      }
      if (!Compressible || Needs.empty())
        continue;

      unsigned CandId = NextCand++;
      Facts << "cand(i" << CandId << "). cand_saving(i" << CandId << ", 2).";
      for (Register Reg : Needs)
        Facts << " needs_gprc(i" << CandId << ", " << getId(Reg) << ").";
      Facts << "\n";
      AnyCandidate = true;
    }
  }

  if (!AnyCandidate)
    return false;

  // Build the GPRC physical-register pool: index i -> GPRCRegs[i].
  std::vector<MCPhysReg> GPRCRegs(RISCV::GPRCRegClass.begin(),
                                  RISCV::GPRCRegClass.end());
  for (unsigned I = 0, E = GPRCRegs.size(); I != E; ++I)
    Facts << "gprc_reg(" << I << ").\n";

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

  for (auto [VIdx, RIdx] : Assignment) {
    if (VIdx >= IdToVReg.size() || RIdx >= GPRCRegs.size())
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
