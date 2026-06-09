//===- RISCVASPRegAlloc.cpp - Single-phase ASP register allocator ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A single-phase, code-size-oriented register allocator for RISC-V, built on
// top of the greedy allocator.  It runs one whole-function Answer Set
// Programming solve that colors the GPR interference graph to maximize RVC
// (compressed) instructions and eliminated copies, then *binds* that coloring
// by pre-assigning each chosen vreg into LiveRegMatrix before greedy's main
// loop.  Greedy then performs all live-range splitting, eviction and spilling
// for everything the solver did not place -- and, on functions the solver
// cannot color combinatorially (too large, or the solve times out), it simply
// runs as plain greedy.  So the fallback is the state-of-the-art allocator, not
// a basic one.
//
// Binding via pre-assignment (not register-allocation hints) is deliberate: a
// soft hint is a lossy channel that greedy discards under pressure, whereas a
// pre-assignment is realized exactly where it is interference-free.
//
// Selected with -regalloc=riscv-asp-cs.  Default greedy and the phase-1 hint
// pass are unaffected and remain the alternates.
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVSubtarget.h"
#include "RegAllocGreedy.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/EdgeBundles.h"
#include "llvm/CodeGen/LiveDebugVariables.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineOptimizationRemarkEmitter.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/CodeGen/SpillPlacement.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <string>
#include <tuple>
#include <vector>

#ifdef LLVM_PBQP_HAVE_CLINGO
#include <atomic>
#include <chrono>
#include <clingo.h>
#include <cstring>
#include <sstream>
#include <thread>
#endif

using namespace llvm;

#define DEBUG_TYPE "riscv-asp-cs"

STATISTIC(NumASPAssigned, "Vregs bound to their ASP-chosen register");

static cl::opt<unsigned>
    ASPCSTimeLimit("riscv-asp-cs-time-limit", cl::Hidden, cl::init(10),
                   cl::desc("Clingo wall-clock time limit (seconds) for the "
                            "single-phase solve; raise/lower to trade solve "
                            "time against allocation quality"));

// Combinatorial coloring does not scale to very large functions; above this
// many GPR virtual registers, skip the solve and let greedy allocate.
static cl::opt<unsigned>
    ASPCSMaxVRegs("riscv-asp-cs-max-vregs", cl::Hidden, cl::init(80),
                  cl::desc("Skip the ASP solve above this many GPR vregs"));

namespace {

//===----------------------------------------------------------------------===//
// The allocator: greedy, plus an ASP pre-seeding step in enqueueImpl.
//===----------------------------------------------------------------------===//
class RISCVASPGreedy : public RAGreedy {
  bool Solved = false;
  DenseMap<Register, MCRegister> ASPChoice;

  // Build and solve the single-phase model from the function; fill ASPChoice.
  void solveASP();

public:
  RISCVASPGreedy(RAGreedy::RequiredAnalyses &A,
                 const RegAllocFilterFunc F = nullptr)
      : RAGreedy(A, F) {}

  // Trigger the solve once, on the first enqueue (after init(), so liveness and
  // the matrix are ready), then enqueue normally so greedy initializes its
  // per-vreg bookkeeping (eviction stages etc.) for every vreg.
  void enqueueImpl(const LiveInterval *LI) override {
    if (!Solved) {
      solveASP();
      Solved = true;
    }
    RAGreedy::enqueueImpl(LI);
  }

  // Bind the solver's choice when it is class-legal and interference-free;
  // otherwise fall back to the full greedy selector (eviction / splitting /
  // spilling).  Binding here (a committed assignment) rather than via a soft
  // hint is deliberate: hints are discarded under pressure, an assignment is
  // not.  Functions the solver did not color produce no choices -> pure greedy.
  MCRegister selectOrSplit(const LiveInterval &VirtReg,
                           SmallVectorImpl<Register> &NewVRegs) override {
    auto It = ASPChoice.find(VirtReg.reg());
    if (It != ASPChoice.end()) {
      MCRegister P = It->second;
      if (MRI->getRegClass(VirtReg.reg())->contains(P) &&
          Matrix->checkInterference(VirtReg, P) == LiveRegMatrix::IK_Free) {
        ++NumASPAssigned;
        return P;
      }
    }
    return RAGreedy::selectOrSplit(VirtReg, NewVRegs);
  }
};

//===----------------------------------------------------------------------===//
// Legacy pass wrapper: gathers analyses and drives RISCVASPGreedy (mirrors
// RAGreedyLegacy so the -regalloc registry can select it).
//===----------------------------------------------------------------------===//
class RISCVASPRegAlloc : public MachineFunctionPass {
public:
  static char ID;
  RISCVASPRegAlloc() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "RISC-V single-phase ASP register allocator";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<MachineBlockFrequencyInfoWrapperPass>();
    AU.addPreserved<MachineBlockFrequencyInfoWrapperPass>();
    AU.addRequired<LiveIntervalsWrapperPass>();
    AU.addPreserved<LiveIntervalsWrapperPass>();
    AU.addRequired<SlotIndexesWrapperPass>();
    AU.addPreserved<SlotIndexesWrapperPass>();
    AU.addRequired<LiveDebugVariablesWrapperLegacy>();
    AU.addPreserved<LiveDebugVariablesWrapperLegacy>();
    AU.addRequired<LiveStacksWrapperLegacy>();
    AU.addPreserved<LiveStacksWrapperLegacy>();
    AU.addRequired<MachineDominatorTreeWrapperPass>();
    AU.addPreserved<MachineDominatorTreeWrapperPass>();
    AU.addRequired<MachineLoopInfoWrapperPass>();
    AU.addPreserved<MachineLoopInfoWrapperPass>();
    AU.addRequired<VirtRegMapWrapperLegacy>();
    AU.addPreserved<VirtRegMapWrapperLegacy>();
    AU.addRequired<LiveRegMatrixWrapperLegacy>();
    AU.addPreserved<LiveRegMatrixWrapperLegacy>();
    AU.addRequired<EdgeBundlesWrapperLegacy>();
    AU.addRequired<SpillPlacementWrapperLegacy>();
    AU.addRequired<MachineOptimizationRemarkEmitterPass>();
    AU.addRequired<RegAllocEvictionAdvisorAnalysisLegacy>();
    AU.addRequired<RegAllocPriorityAdvisorAnalysisLegacy>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    RAGreedy::RequiredAnalyses Analyses(*this);
    RISCVASPGreedy Impl(Analyses);
    return Impl.run(MF);
  }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoPHIs();
  }
  MachineFunctionProperties getClearedProperties() const override {
    return MachineFunctionProperties().setIsSSA();
  }
};

char RISCVASPRegAlloc::ID = 0;

} // end anonymous namespace

INITIALIZE_PASS_BEGIN(RISCVASPRegAlloc, "riscv-asp-cs",
                      "RISC-V single-phase ASP register allocator", false, false)
INITIALIZE_PASS_DEPENDENCY(LiveDebugVariablesWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(SlotIndexesWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(RegisterCoalescerLegacy)
INITIALIZE_PASS_DEPENDENCY(MachineSchedulerLegacy)
INITIALIZE_PASS_DEPENDENCY(LiveStacksWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(VirtRegMapWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(LiveRegMatrixWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(EdgeBundlesWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(SpillPlacementWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(MachineOptimizationRemarkEmitterPass)
INITIALIZE_PASS_DEPENDENCY(RegAllocEvictionAdvisorAnalysisLegacy)
INITIALIZE_PASS_DEPENDENCY(RegAllocPriorityAdvisorAnalysisLegacy)
INITIALIZE_PASS_END(RISCVASPRegAlloc, "riscv-asp-cs",
                    "RISC-V single-phase ASP register allocator", false, false)

static RegisterRegAlloc
    aspCSRegAlloc("riscv-asp-cs",
                  "RISC-V single-phase ASP (code-size) register allocator",
                  []() -> FunctionPass * { return new RISCVASPRegAlloc(); });

FunctionPass *llvm::createRISCVASPRegAlloc() { return new RISCVASPRegAlloc(); }

//===----------------------------------------------------------------------===//
// The single-phase ASP solve.
//===----------------------------------------------------------------------===//

#ifdef LLVM_PBQP_HAVE_CLINGO

// Static policy half of the model.  Dynamic facts are appended per function.
// Greedy owns spilling, so the model colors the interference graph to maximize
// realized compressions and eliminated copies, minus callee-saved cost.
static const char *const kASPModel =
    "{ assign(V, R) : reg(R) } 1 :- vreg(V).\n"
    ":- assign(V1, R), assign(V2, R), interfere(V1, V2).\n"
    "gprc_ok(V) :- assign(V, R), gprc(R).\n"
    "blocked(I) :- needs_gprc(I, V), not gprc_ok(V).\n"
    "realized(I) :- cand(I), not blocked(I).\n"
    "coalesced(C) :- copy_phys(C, V, R), assign(V, R).\n"
    "coalesced(C) :- copy_v(C, V1, V2), assign(V1, R), assign(V2, R).\n"
    "used_cs(R) :- assign(_, R), callee_saved(R).\n"
    "#maximize { B@1, realized, I : realized(I), cand_save(I, B) }.\n"
    "#maximize { B@1, coalesced, C : coalesced(C), copy_save(C, B) }.\n"
    "#minimize { N@1, cs, R : used_cs(R), cs_cost(N) }.\n"
    "#show assign/2.\n";

static bool parseAssignAtom(clingo_symbol_t Sym, unsigned &V, unsigned &R) {
  if (clingo_symbol_type(Sym) != clingo_symbol_type_function)
    return false;
  char const *Name = nullptr;
  if (!clingo_symbol_name(Sym, &Name) || std::strcmp(Name, "assign") != 0)
    return false;
  clingo_symbol_t const *Args = nullptr;
  size_t NumArgs = 0;
  if (!clingo_symbol_arguments(Sym, &Args, &NumArgs) || NumArgs != 2)
    return false;
  int VN = 0, RN = 0;
  if (!clingo_symbol_number(Args[0], &VN) || !clingo_symbol_number(Args[1], &RN))
    return false;
  V = (unsigned)VN;
  R = (unsigned)RN;
  return true;
}

static bool runASPSolve(const std::string &Program, unsigned TimeLimitSecs,
                        std::vector<std::pair<unsigned, unsigned>> &Out) {
  clingo_control_t *Ctl = nullptr;
  char const *Args[] = {"--opt-mode=opt"};
  auto Logger = [](clingo_warning_t, char const *, void *) {};
  if (!clingo_control_new(Args, 1, Logger, nullptr, 20, &Ctl))
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
  std::atomic<bool> Done{false};
  std::thread Timeout([&]() {
    auto Deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(TimeLimitSecs);
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
      if (parseAssignAtom(Sym, V, R))
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

void RISCVASPGreedy::solveASP() {
  MachineFunction &MF = VRM->getMachineFunction();
  const RISCVSubtarget &ST = MF.getSubtarget<RISCVSubtarget>();
  if (!ST.hasStdExtZca())
    return; // no compressed instruction forms available.

  ArrayRef<MCPhysReg> Pool = RegClassInfo.getOrder(&RISCV::GPRRegClass);
  if (Pool.empty())
    return;
  DenseMap<MCRegister, unsigned> RegId;
  for (unsigned I = 0; I < Pool.size(); ++I)
    RegId[Pool[I]] = I;

  DenseMap<Register, unsigned> VRegId;
  std::vector<Register> IdToVReg;
  auto getVId = [&](Register R) -> int {
    auto It = VRegId.find(R);
    if (It != VRegId.end())
      return (int)It->second;
    if (!R.isVirtual() || !LIS->hasInterval(R) ||
        !MRI->getRegClass(R)->contains(RISCV::X10))
      return -1;
    unsigned Id = IdToVReg.size();
    VRegId[R] = Id;
    IdToVReg.push_back(R);
    return (int)Id;
  };

  auto immOk = [](const MachineInstr &MI, unsigned OpNo, auto Pred) -> bool {
    return MI.getOperand(OpNo).isImm() &&
           Pred((int64_t)MI.getOperand(OpNo).getImm());
  };
  auto opOK = [&](const MachineOperand &MO, SmallVectorImpl<Register> &Need,
                  bool &Reject) {
    if (!MO.isReg()) {
      Reject = true;
      return;
    }
    Register R = MO.getReg();
    if (R.isPhysical()) {
      if (!RISCV::GPRCRegClass.contains(R))
        Reject = true;
      return;
    }
    Need.push_back(R);
  };
  auto classify = [&](const MachineInstr &MI,
                      SmallVectorImpl<Register> &Need) -> bool {
    bool Reject = false;
    unsigned Op = MI.getOpcode();
    auto isLoadStore = [&](auto ImmPred, bool Zcb) -> bool {
      if (Zcb && !ST.hasStdExtZcb())
        return false;
      if (!immOk(MI, 2, ImmPred))
        return false;
      opOK(MI.getOperand(0), Need, Reject);
      opOK(MI.getOperand(1), Need, Reject);
      return !Reject && !Need.empty();
    };
    switch (Op) {
    case RISCV::LW:
    case RISCV::SW:
      return isLoadStore([](int64_t I) { return isShiftedUInt<5, 2>(I); }, false);
    case RISCV::LD:
    case RISCV::SD:
      return isLoadStore([](int64_t I) { return isShiftedUInt<5, 3>(I); }, false);
    case RISCV::AND:
    case RISCV::OR:
    case RISCV::XOR:
    case RISCV::SUB:
    case RISCV::ADDW:
    case RISCV::SUBW: {
      if (!MI.getOperand(0).isReg() || !MI.getOperand(1).isReg() ||
          MI.getOperand(0).getReg() != MI.getOperand(1).getReg())
        return false;
      opOK(MI.getOperand(0), Need, Reject);
      opOK(MI.getOperand(2), Need, Reject);
      return !Reject && !Need.empty();
    }
    case RISCV::ANDI:
      if (!immOk(MI, 2, [](int64_t I) { return isInt<6>(I); }))
        return false;
      [[fallthrough]];
    case RISCV::SRLI:
    case RISCV::SRAI: {
      if (Op != RISCV::ANDI &&
          !immOk(MI, 2, [](int64_t I) { return I >= 1 && I <= 63; }))
        return false;
      if (!MI.getOperand(0).isReg() || !MI.getOperand(1).isReg() ||
          MI.getOperand(0).getReg() != MI.getOperand(1).getReg())
        return false;
      opOK(MI.getOperand(0), Need, Reject);
      return !Reject && !Need.empty();
    }
    default:
      return false;
    }
  };

  std::ostringstream F;
  std::vector<std::pair<const MachineInstr *, SmallVector<Register, 3>>> Cands;
  for (const MachineBasicBlock &MBB : MF)
    for (const MachineInstr &MI : MBB) {
      SmallVector<Register, 3> Need;
      if (!classify(MI, Need))
        continue;
      bool Ok = true;
      for (Register R : Need)
        if (getVId(R) < 0) {
          Ok = false;
          break;
        }
      if (Ok)
        Cands.emplace_back(&MI, std::move(Need));
    }

  std::vector<std::tuple<bool, unsigned, unsigned>> Copies; // (phys, A, B)
  for (const MachineBasicBlock &MBB : MF)
    for (const MachineInstr &MI : MBB) {
      if (!MI.isCopy())
        continue;
      Register Dst = MI.getOperand(0).getReg();
      Register Src = MI.getOperand(1).getReg();
      if (Dst.isVirtual() && Src.isPhysical()) {
        auto RIt = RegId.find(Src);
        int V = getVId(Dst);
        if (RIt != RegId.end() && V >= 0)
          Copies.emplace_back(true, (unsigned)V, RIt->second);
      } else if (Src.isVirtual() && Dst.isPhysical()) {
        auto RIt = RegId.find(Dst);
        int V = getVId(Src);
        if (RIt != RegId.end() && V >= 0)
          Copies.emplace_back(true, (unsigned)V, RIt->second);
      } else if (Dst.isVirtual() && Src.isVirtual()) {
        int A = getVId(Dst), B = getVId(Src);
        if (A >= 0 && B >= 0 && A != B)
          Copies.emplace_back(false, (unsigned)A, (unsigned)B);
      }
    }

  if (IdToVReg.empty() || IdToVReg.size() > ASPCSMaxVRegs)
    return;

  const MCPhysReg *CSR = MRI->getCalleeSavedRegs();
  auto isCalleeSaved = [&](MCPhysReg R) {
    for (const MCPhysReg *P = CSR; P && *P; ++P)
      if (*P == R)
        return true;
    return false;
  };
  for (unsigned I = 0; I < Pool.size(); ++I) {
    F << "reg(" << I << ").";
    if (RISCV::GPRCRegClass.contains(Pool[I]))
      F << " gprc(" << I << ").";
    if (isCalleeSaved(Pool[I]))
      F << " callee_saved(" << I << ").";
    F << "\n";
  }
  unsigned CsCost = (ST.hasStdExtZcmp() || ST.enableSaveRestore()) ? 1 : 4;
  F << "cs_cost(" << CsCost << ").\n";
  for (unsigned I = 0; I < IdToVReg.size(); ++I)
    F << "vreg(" << I << ").\n";
  for (unsigned A = 0; A < IdToVReg.size(); ++A)
    for (unsigned B = A + 1; B < IdToVReg.size(); ++B)
      if (LIS->getInterval(IdToVReg[A]).overlaps(LIS->getInterval(IdToVReg[B])))
        F << "interfere(" << A << ", " << B << ").\n";
  unsigned CandId = 0;
  for (auto &C : Cands) {
    F << "cand(i" << CandId << "). cand_save(i" << CandId << ", 2).";
    for (Register R : C.second)
      F << " needs_gprc(i" << CandId << ", " << VRegId[R] << ").";
    F << "\n";
    ++CandId;
  }
  unsigned CopyId = 0;
  for (auto &[Phys, A, B] : Copies) {
    F << (Phys ? "copy_phys(c" : "copy_v(c") << CopyId << ", " << A << ", " << B
      << "). copy_save(c" << CopyId << ", 2).\n";
    ++CopyId;
  }

  std::string Program = std::string(kASPModel) + F.str();
  LLVM_DEBUG(dbgs() << "RISCV-ASP-CS program for " << MF.getName() << ":\n"
                    << Program << "\n");

  std::vector<std::pair<unsigned, unsigned>> Assignment;
  if (!runASPSolve(Program, ASPCSTimeLimit, Assignment))
    return;

  for (auto [VIdx, RIdx] : Assignment)
    if (VIdx < IdToVReg.size() && RIdx < Pool.size())
      ASPChoice[IdToVReg[VIdx]] = Pool[RIdx];
  LLVM_DEBUG(dbgs() << "RISCV-ASP-CS: " << ASPChoice.size() << " bindings for "
                    << MF.getName() << "\n");
}

#else

void RISCVASPGreedy::solveASP() {}

#endif // LLVM_PBQP_HAVE_CLINGO
