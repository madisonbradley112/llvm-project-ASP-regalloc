//===- RISCVASPRegAlloc.cpp - Single-phase ASP register allocator ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A single-phase, code-size-oriented register allocator for RISC-V.  Unlike the
// phase-1 hint pass (RISCVRVCRegAllocHints), which only nudges the greedy
// allocator, this *is* the allocator: it performs one whole-function Answer Set
// Programming solve that jointly chooses register assignments while maximizing
// RVC (compressed) instructions, then realizes that assignment using LLVM's
// standard allocation machinery (VirtRegMap / LiveRegMatrix / InlineSpiller),
// spilling whatever does not fit -- exactly as the basic allocator does.
//
// It is built on RegAllocBase (like RABasic) so the proven spill/rewrite path
// is reused unchanged.  The ASP solution is consulted as a *binding preference*
// in selectOrSplit: a virtual register is placed in its solver-chosen physical
// register whenever that register is interference-free; otherwise the allocator
// falls back to the basic select-or-spill logic, guaranteeing correctness and
// forward progress regardless of what the solver returned (or if it timed out,
// or if LLVM was built without Clingo).
//
// Selected with -regalloc=riscv-asp-cs.  The default greedy allocator and the
// phase-1 hint pass are unaffected and remain the alternates.
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVSubtarget.h"
#include "RegAllocBase.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/CalcSpillWeights.h"
#include "llvm/CodeGen/LiveDebugVariables.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRangeEdit.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/Spiller.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <queue>
#include <string>
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

STATISTIC(NumASPAssigned, "Vregs placed in their ASP-chosen register");
STATISTIC(NumASPFallback, "Vregs that fell back to basic allocation");

static cl::opt<unsigned>
    ASPCSTimeLimit("riscv-asp-cs-time-limit", cl::Hidden, cl::init(10),
                   cl::desc("Clingo time limit (s) for the single-phase solve"));

// Combinatorial coloring does not scale to very large functions; above this
// many GPR virtual registers, skip the solve and allocate basically.
static cl::opt<unsigned>
    ASPCSMaxVRegs("riscv-asp-cs-max-vregs", cl::Hidden, cl::init(80),
                  cl::desc("Skip the ASP solve above this many GPR vregs"));

namespace {

struct CompSpillWeight {
  bool operator()(const LiveInterval *A, const LiveInterval *B) const {
    return A->weight() < B->weight();
  }
};

class RISCVASPRegAlloc : public MachineFunctionPass,
                         public RegAllocBase,
                         private LiveRangeEdit::Delegate {
  MachineFunction *MF = nullptr;
  std::unique_ptr<Spiller> SpillerInstance;
  std::priority_queue<const LiveInterval *, std::vector<const LiveInterval *>,
                      CompSpillWeight>
      Queue;

  /// Solver-chosen physical register for each original virtual register.
  DenseMap<Register, MCRegister> ASPChoice;

  bool LRE_CanEraseVirtReg(Register) override;
  void LRE_WillShrinkVirtReg(Register) override;

  // Build and solve the single-phase ASP model; fill ASPChoice.  A no-op (no
  // preferences) when built without Clingo or when the solve yields nothing.
  void solveASP();

public:
  static char ID;

  RISCVASPRegAlloc() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "RISC-V single-phase ASP register allocator";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void releaseMemory() override { SpillerInstance.reset(); }

  Spiller &spiller() override { return *SpillerInstance; }
  void enqueueImpl(const LiveInterval *LI) override { Queue.push(LI); }
  const LiveInterval *dequeue() override {
    if (Queue.empty())
      return nullptr;
    const LiveInterval *LI = Queue.top();
    Queue.pop();
    return LI;
  }

  MCRegister selectOrSplit(const LiveInterval &VirtReg,
                           SmallVectorImpl<Register> &SplitVRegs) override;

  bool runOnMachineFunction(MachineFunction &mf) override;

  bool spillInterferences(const LiveInterval &VirtReg, MCRegister PhysReg,
                          SmallVectorImpl<Register> &SplitVRegs);

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::NoPHIs);
  }
  MachineFunctionProperties getClearedProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::IsSSA);
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
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(VirtRegMapWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(LiveRegMatrixWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(ProfileSummaryInfoWrapperPass)
INITIALIZE_PASS_END(RISCVASPRegAlloc, "riscv-asp-cs",
                    "RISC-V single-phase ASP register allocator", false, false)

static RegisterRegAlloc
    aspCSRegAlloc("riscv-asp-cs",
                  "RISC-V single-phase ASP (code-size) register allocator",
                  []() -> FunctionPass * { return new RISCVASPRegAlloc(); });

void RISCVASPRegAlloc::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addPreserved<AAResultsWrapperPass>();
  AU.addRequired<LiveIntervalsWrapperPass>();
  AU.addPreserved<LiveIntervalsWrapperPass>();
  AU.addPreserved<SlotIndexesWrapperPass>();
  AU.addRequired<LiveDebugVariablesWrapperLegacy>();
  AU.addPreserved<LiveDebugVariablesWrapperLegacy>();
  AU.addRequired<LiveStacksWrapperLegacy>();
  AU.addPreserved<LiveStacksWrapperLegacy>();
  AU.addRequired<ProfileSummaryInfoWrapperPass>();
  AU.addRequired<MachineBlockFrequencyInfoWrapperPass>();
  AU.addPreserved<MachineBlockFrequencyInfoWrapperPass>();
  AU.addRequired<MachineDominatorTreeWrapperPass>();
  AU.addRequiredID(MachineDominatorsID);
  AU.addPreservedID(MachineDominatorsID);
  AU.addRequired<MachineLoopInfoWrapperPass>();
  AU.addPreserved<MachineLoopInfoWrapperPass>();
  AU.addRequired<VirtRegMapWrapperLegacy>();
  AU.addPreserved<VirtRegMapWrapperLegacy>();
  AU.addRequired<LiveRegMatrixWrapperLegacy>();
  AU.addPreserved<LiveRegMatrixWrapperLegacy>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

bool RISCVASPRegAlloc::LRE_CanEraseVirtReg(Register VirtReg) {
  LiveInterval &LI = LIS->getInterval(VirtReg);
  if (VRM->hasPhys(VirtReg)) {
    Matrix->unassign(LI);
    aboutToRemoveInterval(LI);
    return true;
  }
  LI.clear();
  return false;
}

void RISCVASPRegAlloc::LRE_WillShrinkVirtReg(Register VirtReg) {
  if (!VRM->hasPhys(VirtReg))
    return;
  LiveInterval &LI = LIS->getInterval(VirtReg);
  Matrix->unassign(LI);
  enqueue(&LI);
}

// Identical to RABasic::spillInterferences.
bool RISCVASPRegAlloc::spillInterferences(
    const LiveInterval &VirtReg, MCRegister PhysReg,
    SmallVectorImpl<Register> &SplitVRegs) {
  SmallVector<const LiveInterval *, 8> Intfs;
  for (MCRegUnit Unit : TRI->regunits(PhysReg)) {
    LiveIntervalUnion::Query &Q = Matrix->query(VirtReg, Unit);
    for (const auto *Intf : reverse(Q.interferingVRegs())) {
      if (!Intf->isSpillable() || Intf->weight() > VirtReg.weight())
        return false;
      Intfs.push_back(Intf);
    }
  }
  for (const LiveInterval *Spill : Intfs) {
    if (!VRM->hasPhys(Spill->reg()))
      continue;
    Matrix->unassign(*Spill);
    LiveRangeEdit LRE(Spill, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats);
    spiller().spill(LRE);
  }
  return true;
}

MCRegister
RISCVASPRegAlloc::selectOrSplit(const LiveInterval &VirtReg,
                                SmallVectorImpl<Register> &SplitVRegs) {
  // 1) Binding preference: if the solver chose a register for this vreg and it
  //    is interference-free right now, use it.
  auto It = ASPChoice.find(VirtReg.reg());
  if (It != ASPChoice.end()) {
    MCRegister Pref = It->second;
    // Only honor the solver's choice if it is legal for this vreg's register
    // class and currently interference-free; otherwise fall back.
    if (MRI->getRegClass(VirtReg.reg())->contains(Pref) &&
        Matrix->checkInterference(VirtReg, Pref) == LiveRegMatrix::IK_Free) {
      ++NumASPAssigned;
      return Pref;
    }
  }

  // 2) Basic select-or-split fallback.  Iterate the class allocation order
  //    directly (AllocationOrder is a CodeGen-internal symbol not exported to
  //    target libraries; RegClassInfo::getOrder gives the same register order).
  ++NumASPFallback;
  SmallVector<MCRegister, 8> PhysRegSpillCands;
  ArrayRef<MCPhysReg> Order =
      RegClassInfo.getOrder(MRI->getRegClass(VirtReg.reg()));
  for (MCRegister PhysReg : Order) {
    switch (Matrix->checkInterference(VirtReg, PhysReg)) {
    case LiveRegMatrix::IK_Free:
      return PhysReg;
    case LiveRegMatrix::IK_VirtReg:
      PhysRegSpillCands.push_back(PhysReg);
      continue;
    default:
      continue;
    }
  }
  for (MCRegister &PhysReg : PhysRegSpillCands) {
    if (!spillInterferences(VirtReg, PhysReg, SplitVRegs))
      continue;
    return PhysReg;
  }
  if (!VirtReg.isSpillable())
    return ~0u;
  LiveRangeEdit LRE(&VirtReg, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats);
  spiller().spill(LRE);
  return 0;
}

bool RISCVASPRegAlloc::runOnMachineFunction(MachineFunction &mf) {
  LLVM_DEBUG(dbgs() << "********** RISC-V ASP REGISTER ALLOCATION **********\n"
                    << "********** Function: " << mf.getName() << '\n');
  MF = &mf;
  auto &MBFI = getAnalysis<MachineBlockFrequencyInfoWrapperPass>().getMBFI();
  auto &LiveStks = getAnalysis<LiveStacksWrapperLegacy>().getLS();
  auto &MDT = getAnalysis<MachineDominatorTreeWrapperPass>().getDomTree();

  RegAllocBase::init(getAnalysis<VirtRegMapWrapperLegacy>().getVRM(),
                     getAnalysis<LiveIntervalsWrapperPass>().getLIS(),
                     getAnalysis<LiveRegMatrixWrapperLegacy>().getLRM());
  VirtRegAuxInfo VRAI(*MF, *LIS, *VRM,
                      getAnalysis<MachineLoopInfoWrapperPass>().getLI(), MBFI,
                      &getAnalysis<ProfileSummaryInfoWrapperPass>().getPSI());
  VRAI.calculateSpillWeightsAndHints();

  SpillerInstance.reset(
      createInlineSpiller({*LIS, LiveStks, MDT, MBFI}, *MF, *VRM, VRAI));

  ASPChoice.clear();
  solveASP();

  allocatePhysRegs();
  postOptimization();
  releaseMemory();
  return true;
}

//===----------------------------------------------------------------------===//
// The single-phase ASP solve.  Builds the model from the function and fills
// ASPChoice with a solver-chosen physical register per original vreg.
//===----------------------------------------------------------------------===//

#ifdef LLVM_PBQP_HAVE_CLINGO

// Static policy half of the single-phase model.  Dynamic facts (reg/1, gprc/1,
// vreg/1, interfere/2, cand/1, cand_save/2, needs_gprc/2) are appended per
// function.  The allocator owns spilling, so the model only colors the
// interference graph to maximize realized compressions.
static const char *const kASPModel =
    "{ assign(V, R) : reg(R) } 1 :- vreg(V).\n"
    ":- assign(V1, R), assign(V2, R), interfere(V1, V2).\n"
    "gprc_ok(V) :- assign(V, R), gprc(R).\n"
    "blocked(I) :- needs_gprc(I, V), not gprc_ok(V).\n"
    "realized(I) :- cand(I), not blocked(I).\n"
    "#maximize { B@1, realized, I : realized(I), cand_save(I, B) }.\n"
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

// Solve kASPModel + Facts, keeping the optimal model's assign/2 atoms.
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

void RISCVASPRegAlloc::solveASP() {
  const RISCVSubtarget &ST = MF->getSubtarget<RISCVSubtarget>();
  if (!ST.hasStdExtZca())
    return; // no compressed instruction forms available.

  // GPR allocation pool (dense id -> physreg), with GPRC members marked.
  ArrayRef<MCPhysReg> Pool = RegClassInfo.getOrder(&RISCV::GPRRegClass);
  if (Pool.empty())
    return;
  DenseMap<MCRegister, unsigned> RegId;
  for (unsigned I = 0; I < Pool.size(); ++I)
    RegId[Pool[I]] = I;

  // GPR virtual registers to color (dense id).
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

  // Classify a compressible instruction: returns true and fills NeedVRegs with
  // the virtual operands that must be in GPRC, or false if not a (realizable)
  // candidate.  A fixed non-GPRC physreg operand rejects the candidate; a fixed
  // GPRC physreg operand is already satisfied and simply omitted.
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
      return; // fixed GPRC physreg: already satisfied.
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
    // Reg-reg two-address ALU: compresses only in the coalesced rd==rs1 form.
    case RISCV::AND:
    case RISCV::OR:
    case RISCV::XOR:
    case RISCV::SUB:
    case RISCV::ADDW:
    case RISCV::SUBW: {
      if (!MI.getOperand(0).isReg() || !MI.getOperand(1).isReg() ||
          MI.getOperand(0).getReg() != MI.getOperand(1).getReg())
        return false; // not coalesced two-address -> cannot compress.
      opOK(MI.getOperand(0), Need, Reject);
      opOK(MI.getOperand(2), Need, Reject);
      return !Reject && !Need.empty();
    }
    // Reg-imm two-address.
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

  // ---- Build facts ---------------------------------------------------------
  std::ostringstream F;
  std::vector<std::pair<const MachineInstr *, SmallVector<Register, 3>>> Cands;
  for (const MachineBasicBlock &MBB : *MF)
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

  if (IdToVReg.empty() || IdToVReg.size() > ASPCSMaxVRegs)
    return; // nothing to do, or too large to color combinatorially.

  for (unsigned I = 0; I < Pool.size(); ++I) {
    F << "reg(" << I << ").";
    if (RISCV::GPRCRegClass.contains(Pool[I]))
      F << " gprc(" << I << ").";
    F << "\n";
  }
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

  std::string Program = std::string(kASPModel) + F.str();
  LLVM_DEBUG(dbgs() << "RISCV-ASP-CS program for " << MF->getName() << ":\n"
                    << Program << "\n");

  std::vector<std::pair<unsigned, unsigned>> Assignment;
  if (!runASPSolve(Program, ASPCSTimeLimit, Assignment))
    return; // UNSAT / timeout / no model: basic allocation.

  for (auto [VIdx, RIdx] : Assignment)
    if (VIdx < IdToVReg.size() && RIdx < Pool.size())
      ASPChoice[IdToVReg[VIdx]] = Pool[RIdx];
  LLVM_DEBUG(dbgs() << "RISCV-ASP-CS: " << ASPChoice.size() << " preferences for "
                    << MF->getName() << "\n");
}

#else

void RISCVASPRegAlloc::solveASP() {}

#endif // LLVM_PBQP_HAVE_CLINGO

FunctionPass *llvm::createRISCVASPRegAlloc() { return new RISCVASPRegAlloc(); }
