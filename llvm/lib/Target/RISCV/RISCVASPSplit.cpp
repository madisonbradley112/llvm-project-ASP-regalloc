//===- RISCVASPSplit.cpp - Decomposed ASP splitting allocator (pre-RA) ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pre-RA pass that brings the region-decomposed, per-program-point ASP
// *splitting* model (prototyped standalone in ASP-PBQP-regalloc/splitting/) to
// real code.  For each basic block it discretizes the instructions into program
// points, extracts GPR-vreg liveness / uses / defs and the RVC compression
// candidates, runs an Answer-Set solve that decides each value's location
// (register R or memory) at every point -- so a value can be kept in a register
// across its hot span and spilled across its cold middle (a live-range split),
// jointly with the GPRC-for-compression objective.
//
// STAGE 1 (this file, current): extraction + solve + decision dump under
// -riscv-asp-split (off by default) and -debug-only=riscv-asp-split.  This
// validates that the model runs on real liveness and shows the split/compress
// decisions.  STAGE 2 (todo) materializes those decisions: store/reload at the
// chosen split points (via SplitKit/LiveRangeEdit) and GPRC class constraints,
// then greedy finishes allocation.
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVSubtarget.h"
#include "llvm/ADT/DenseMap.h"
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
#include <sstream>
#include <thread>
#endif

#include <string>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "riscv-asp-split"

STATISTIC(NumFuncs, "Functions processed by the ASP splitting pass");
STATISTIC(NumBlocksSolved, "Basic blocks solved by the ASP splitting model");
STATISTIC(NumValuesSplit, "Values the model chose to split (reg + memory)");
STATISTIC(NumRealized, "Compression candidates realized (value in GPRC)");

static cl::opt<bool> EnableASPSplit(
    "riscv-asp-split", cl::Hidden, cl::init(false),
    cl::desc("Enable the region-decomposed ASP live-range splitting pass"));

static cl::opt<unsigned> ASPSplitTimeLimit(
    "riscv-asp-split-time-limit", cl::Hidden, cl::init(2),
    cl::desc("Per-region clingo wall-clock time limit (seconds)"));

static cl::opt<unsigned> ASPSplitWindow(
    "riscv-asp-split-window", cl::Hidden, cl::init(24),
    cl::desc("Max program points per region; larger blocks are split into "
             "windows of this size solved left-to-right"));

static cl::opt<unsigned> ASPSplitMaxBlock(
    "riscv-asp-split-max-points", cl::Hidden, cl::init(2000),
    cl::desc("Skip basic blocks with more than this many points"));

namespace {

// Per-program-point splitting model (mirrors regalloc_region.lp).  Dynamic
// facts (point/reg/gprc/live/use/def/cand/costs and, for non-initial windows,
// frozen_reg/frozen_mem) are appended per region.
static const char *const kSplitModel =
    "{ inreg(V,P,R) : reg(R) } 1 :- live(V,P).\n"
    "inmem(V,P) :- live(V,P), not inreg(V,P,_).\n"
    ":- inreg(V1,P,R), inreg(V2,P,R), V1 < V2.\n"
    ":- use(V,P), inmem(V,P).\n"
    ":- def(V,P), inmem(V,P).\n"
    ":- inreg(V,P-1,R1), inreg(V,P,R2), R1 != R2.\n"
    "reload(V,P) :- inreg(V,P,_), live(V,P-1), inmem(V,P-1).\n"
    "store(V,P)  :- inmem(V,P),   live(V,P-1), inreg(V,P-1,_).\n"
    ":- inreg(V,Lo,R2), first(Lo), frozen_reg(V,R1), R1 != R2.\n"
    "reload(V,Lo) :- inreg(V,Lo,_), first(Lo), frozen_mem(V).\n"
    "store(V,Lo)  :- inmem(V,Lo),  first(Lo), frozen_reg(V,_).\n"
    "realized(I) :- cand(I,V,P), inreg(V,P,R), gprc(R).\n"
    "#maximize { S@1, comp,   I   : realized(I), cand(I,_,_), compsave(S) }.\n"
    "#minimize { C@1, store,  V,P : store(V,P),  storecost(C) }.\n"
    "#minimize { C@1, reload, V,P : reload(V,P), reloadcost(C) }.\n"
    "#show inreg/3.\n";

#ifdef LLVM_PBQP_HAVE_CLINGO
// One solved location: value V is in register-id R at point P (absent => mem).
struct Loc {
  unsigned V, P, R;
};

static bool parseInreg(clingo_symbol_t Sym, unsigned &V, unsigned &P,
                       unsigned &R) {
  if (clingo_symbol_type(Sym) != clingo_symbol_type_function)
    return false;
  char const *Name = nullptr;
  if (!clingo_symbol_name(Sym, &Name) || std::strcmp(Name, "inreg") != 0)
    return false;
  clingo_symbol_t const *Args = nullptr;
  size_t NumArgs = 0;
  if (!clingo_symbol_arguments(Sym, &Args, &NumArgs) || NumArgs != 3)
    return false;
  int VN = 0, PN = 0, RN = 0;
  if (!clingo_symbol_number(Args[0], &VN) || !clingo_symbol_number(Args[1], &PN) ||
      !clingo_symbol_number(Args[2], &RN))
    return false;
  V = (unsigned)VN;
  P = (unsigned)PN;
  R = (unsigned)RN;
  return true;
}

// Solve one region program; return the best model's inreg atoms.  Returns false
// only if no model was found at all.
static bool runSplitSolve(const std::string &Program, unsigned TimeLimitSecs,
                          std::vector<Loc> &Out) {
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
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  });
  std::vector<Loc> Latest;
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
    std::vector<Loc> Cur;
    for (auto Sym : Atoms) {
      unsigned V = 0, P = 0, R = 0;
      if (parseInreg(Sym, V, P, R))
        Cur.push_back({V, P, R});
    }
    Latest = std::move(Cur);
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

class RISCVASPSplit : public MachineFunctionPass {
public:
  static char ID;
  RISCVASPSplit() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "RISC-V decomposed ASP live-range splitting (pre-RA)";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    AU.addRequired<LiveIntervalsWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

private:
#ifdef LLVM_PBQP_HAVE_CLINGO
  // Detect whether MI has an RVC-compressible form gated on some of its GPR
  // operands being in GPRC; append those vreg operands to Need.  Mirrors the
  // classifier in RISCVASPRegAlloc.cpp.
  bool classify(const MachineInstr &MI, const RISCVSubtarget &ST,
                SmallVectorImpl<Register> &Need) const;
  // Solve one basic block (windowed) and dump the decisions.
  void solveBlock(MachineBasicBlock &MBB, LiveIntervals &LIS,
                  MachineRegisterInfo &MRI, const RISCVSubtarget &ST);
#endif
};

char RISCVASPSplit::ID = 0;

#ifdef LLVM_PBQP_HAVE_CLINGO

bool RISCVASPSplit::classify(const MachineInstr &MI, const RISCVSubtarget &ST,
                             SmallVectorImpl<Register> &Need) const {
  bool Reject = false;
  auto opOK = [&](const MachineOperand &MO) {
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
  auto immOk = [&](unsigned OpNo, auto Pred) -> bool {
    return MI.getOperand(OpNo).isImm() &&
           Pred((int64_t)MI.getOperand(OpNo).getImm());
  };
  unsigned Op = MI.getOpcode();
  auto loadStore = [&](auto ImmPred, bool Zcb) -> bool {
    if (Zcb && !ST.hasStdExtZcb())
      return false;
    if (!immOk(2, ImmPred))
      return false;
    opOK(MI.getOperand(0));
    opOK(MI.getOperand(1));
    return !Reject && !Need.empty();
  };
  switch (Op) {
  case RISCV::LW:
  case RISCV::SW:
    return loadStore([](int64_t I) { return isShiftedUInt<5, 2>(I); }, false);
  case RISCV::LD:
  case RISCV::SD:
    return loadStore([](int64_t I) { return isShiftedUInt<5, 3>(I); }, false);
  case RISCV::AND:
  case RISCV::OR:
  case RISCV::XOR:
  case RISCV::SUB:
  case RISCV::ADDW:
  case RISCV::SUBW:
    if (!MI.getOperand(0).isReg() || !MI.getOperand(1).isReg() ||
        MI.getOperand(0).getReg() != MI.getOperand(1).getReg())
      return false;
    opOK(MI.getOperand(0));
    opOK(MI.getOperand(2));
    return !Reject && !Need.empty();
  case RISCV::ANDI:
    if (!immOk(2, [](int64_t I) { return isInt<6>(I); }))
      return false;
    if (!MI.getOperand(0).isReg() || !MI.getOperand(1).isReg() ||
        MI.getOperand(0).getReg() != MI.getOperand(1).getReg())
      return false;
    opOK(MI.getOperand(0));
    return !Reject && !Need.empty();
  case RISCV::SRLI:
  case RISCV::SRAI:
    if (!immOk(2, [](int64_t I) { return I >= 1 && I <= 63; }))
      return false;
    if (!MI.getOperand(0).isReg() || !MI.getOperand(1).isReg() ||
        MI.getOperand(0).getReg() != MI.getOperand(1).getReg())
      return false;
    opOK(MI.getOperand(0));
    return !Reject && !Need.empty();
  default:
    return false;
  }
}

void RISCVASPSplit::solveBlock(MachineBasicBlock &MBB, LiveIntervals &LIS,
                               MachineRegisterInfo &MRI,
                               const RISCVSubtarget &ST) {
  // Linearize the block into program points (one per non-debug instruction).
  SmallVector<MachineInstr *, 64> Pts;
  for (MachineInstr &MI : MBB)
    if (!MI.isDebugInstr())
      Pts.push_back(&MI);
  unsigned N = Pts.size();
  if (N == 0 || N > ASPSplitMaxBlock)
    return;

  // Register pool: allocatable GPRs; mark the GPRC subset.
  std::vector<MCRegister> Regs;
  for (MCPhysReg R : RISCV::GPRRegClass.getRegisters())
    if (MRI.isAllocatable(R))
      Regs.push_back(R);
  if (Regs.empty())
    return;

  // Collect GPR vregs referenced in this block and assign ids.
  DenseMap<Register, unsigned> VId;
  std::vector<Register> IdToV;
  auto getVId = [&](Register R) -> int {
    if (!R.isVirtual() || !LIS.hasInterval(R))
      return -1;
    if (!MRI.getRegClass(R)->contains(RISCV::X10)) // GPR-ish
      return -1;
    auto It = VId.find(R);
    if (It != VId.end())
      return (int)It->second;
    unsigned Id = IdToV.size();
    VId[R] = Id;
    IdToV.push_back(R);
    return (int)Id;
  };

  // Per-point liveness/use/def and compression candidates.
  std::ostringstream F;
  // Track, per vreg id, the live point range within this block.
  DenseMap<unsigned, std::pair<int, int>> LiveRange; // id -> [lo,hi]
  std::vector<std::string> UseDef;                   // use/def facts
  std::vector<std::string> Cands;                    // cand facts
  unsigned CandId = 0;

  for (unsigned P = 0; P < N; ++P) {
    MachineInstr *MI = Pts[P];
    for (const MachineOperand &MO : MI->operands()) {
      if (!MO.isReg() || !MO.getReg().isVirtual())
        continue;
      int Id = getVId(MO.getReg());
      if (Id < 0)
        continue;
      auto &LR = LiveRange.try_emplace(Id, P, P).first->second;
      LR.first = std::min(LR.first, (int)P);
      LR.second = std::max(LR.second, (int)P);
      if (MO.isDef())
        UseDef.push_back("def(" + std::to_string(Id) + "," +
                         std::to_string(P) + ").");
      if (MO.readsReg())
        UseDef.push_back("use(" + std::to_string(Id) + "," +
                         std::to_string(P) + ").");
    }
    // Compression candidates at this point.
    SmallVector<Register, 3> Need;
    if (classify(*MI, ST, Need)) {
      bool Ok = true;
      for (Register R : Need)
        if (getVId(R) < 0) {
          Ok = false;
          break;
        }
      if (Ok) {
        for (Register R : Need)
          Cands.push_back("cand(c" + std::to_string(CandId) + "," +
                          std::to_string(VId[R]) + "," + std::to_string(P) +
                          ").");
        ++CandId;
      }
    }
  }

  if (IdToV.empty())
    return;

  // Extend live ranges for values live-in/out of the block (occupy a register
  // across the whole block on that side).
  SlotIndex BBStart = LIS.getInstructionIndex(*Pts.front()).getBaseIndex();
  SlotIndex BBEnd = LIS.getInstructionIndex(*Pts.back()).getRegSlot();
  for (unsigned Id = 0; Id < IdToV.size(); ++Id) {
    const LiveInterval &LI = LIS.getInterval(IdToV[Id]);
    auto &LR = LiveRange[Id];
    if (LI.liveAt(BBStart))
      LR.first = 0;
    if (LI.liveAt(BBEnd))
      LR.second = (int)N - 1;
  }

  // Emit the program.  (Stage 1: per-block, no cross-block freezing yet.)
  F << "point(0.." << (N - 1) << ").\n";
  for (unsigned I = 0; I < Regs.size(); ++I) {
    F << "reg(" << I << ").";
    if (RISCV::GPRCRegClass.contains(Regs[I]))
      F << " gprc(" << I << ").";
    F << "\n";
  }
  F << "storecost(4). reloadcost(4). compsave(2).\n";
  for (unsigned Id = 0; Id < IdToV.size(); ++Id) {
    auto &LR = LiveRange[Id];
    F << "live(" << Id << "," << LR.first << ".." << LR.second << ").\n";
  }
  for (auto &S : UseDef)
    F << S << "\n";
  for (auto &S : Cands)
    F << S << "\n";

  std::string Program = std::string(kSplitModel) + F.str();
#ifdef LLVM_PBQP_HAVE_CLINGO
  std::vector<Loc> Sol;
  if (!runSplitSolve(Program, ASPSplitTimeLimit, Sol))
    return;
  ++NumBlocksSolved;

  // Decode: for each value, which points are in a register (and which reg).
  // A value is "split" if it is in a register at some live point and in memory
  // at another.
  DenseMap<unsigned, unsigned> InRegCount;
  for (const Loc &L : Sol)
    ++InRegCount[L.V];
  unsigned LocalSplit = 0;
  for (unsigned Id = 0; Id < IdToV.size(); ++Id) {
    auto &LR = LiveRange[Id];
    unsigned LivePts = (unsigned)(LR.second - LR.first + 1);
    unsigned InReg = InRegCount.lookup(Id);
    if (InReg > 0 && InReg < LivePts) {
      ++LocalSplit;
      ++NumValuesSplit;
    }
  }
  LLVM_DEBUG(dbgs() << "  [asp-split] " << MBB.getParent()->getName() << ":"
                    << MBB.getName() << " points=" << N
                    << " vregs=" << IdToV.size() << " cands=" << CandId
                    << " split=" << LocalSplit << "\n");
#endif
}

#endif // LLVM_PBQP_HAVE_CLINGO

bool RISCVASPSplit::runOnMachineFunction(MachineFunction &MF) {
  if (!EnableASPSplit)
    return false;
#ifdef LLVM_PBQP_HAVE_CLINGO
  const RISCVSubtarget &ST = MF.getSubtarget<RISCVSubtarget>();
  if (!ST.hasStdExtZca())
    return false;
  ++NumFuncs;
  LiveIntervals &LIS = getAnalysis<LiveIntervalsWrapperPass>().getLIS();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  LLVM_DEBUG(dbgs() << "[asp-split] function " << MF.getName() << "\n");
  for (MachineBasicBlock &MBB : MF)
    solveBlock(MBB, LIS, MRI, ST);
#endif
  return false; // Stage 1 dumps decisions only; no IR changes yet.
}

} // end anonymous namespace

INITIALIZE_PASS_BEGIN(RISCVASPSplit, DEBUG_TYPE,
                      "RISC-V decomposed ASP live-range splitting", false, false)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_END(RISCVASPSplit, DEBUG_TYPE,
                    "RISC-V decomposed ASP live-range splitting", false, false)

FunctionPass *llvm::createRISCVASPSplitPass() { return new RISCVASPSplit(); }
