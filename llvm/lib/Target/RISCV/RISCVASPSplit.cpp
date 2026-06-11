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
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
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
    "riscv-asp-split-window", cl::Hidden, cl::init(16),
    cl::desc("Max window size (points) for the binary-search window sizer; "
             "blocks are decomposed into windows solved left-to-right"));

static cl::opt<unsigned> ASPSplitWindowMin(
    "riscv-asp-split-window-min", cl::Hidden, cl::init(4),
    cl::desc("Min window size (points) the binary-search sizer narrows to"));

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
    // Block-local splitting: a value crossing the block boundary must stay in a
    // register there, so any spill is stored AND reloaded within this block
    // (cross-block liveness stays register-resident -- no dangling reloads).
    ":- inmem(V,P), pin(V,P).\n"
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
// only if no model was found at all.  Sets Optimal=true iff clingo proved the
// result optimal (search space exhausted) within the budget -- used by the
// binary-search window sizing to decide whether to widen or narrow.
static bool runSplitSolve(const std::string &Program, unsigned TimeLimitSecs,
                          std::vector<Loc> &Out, bool &Optimal) {
  Optimal = false;
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
  // Whether the optimum was proved (search exhausted, not interrupted/timed out).
  clingo_solve_result_bitset_t Res = 0;
  if (clingo_solve_handle_get(Handle, &Res))
    Optimal = (Res & clingo_solve_result_exhausted) &&
              !(Res & clingo_solve_result_interrupted);
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
    // We rewrite the MIR (insert spill/reload), so we do NOT preserve liveness;
    // LiveIntervals/SlotIndexes are recomputed for the register allocator.
    AU.addRequired<LiveIntervalsWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

private:
#ifdef LLVM_PBQP_HAVE_CLINGO
  // A value to split within a block: store it at each reg->mem transition point
  // and reload it (into a fresh vreg, rewriting later uses) at each mem->reg
  // transition point.  Point indices are into the owning BlockPlan's Pts.
  struct VSplit {
    Register V;
    SmallVector<unsigned, 4> StorePts;  // reg -> mem (store current vreg)
    SmallVector<unsigned, 4> ReloadPts; // mem -> reg (reload into a new vreg)
  };
  struct BlockPlan {
    SmallVector<MachineInstr *, 0> Pts; // program points (MI pointers, stable)
    SmallVector<VSplit, 0> Splits;
  };
  std::vector<BlockPlan> Plans;
  DenseSet<Register> GPRCWanted; // values to constrain to GPRC (compression)

  // Detect whether MI has an RVC-compressible form gated on some of its GPR
  // operands being in GPRC; append those vreg operands to Need.  Mirrors the
  // classifier in RISCVASPRegAlloc.cpp.
  bool classify(const MachineInstr &MI, const RISCVSubtarget &ST,
                SmallVectorImpl<Register> &Need) const;
  // Solve one basic block and record split (store/reload) + GPRC actions.
  void solveBlock(MachineBasicBlock &MBB, LiveIntervals &LIS,
                  MachineRegisterInfo &MRI, const RISCVSubtarget &ST);
  // Apply all recorded actions to the MIR; returns true if anything changed.
  bool applyActions(MachineFunction &MF, const RISCVSubtarget &ST);
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

  // Per-point liveness/use/def and compression candidates (structured so we can
  // emit per-window subsets during the windowed decomposition below).
  DenseMap<unsigned, std::pair<int, int>> LiveRange;    // id -> [lo,hi]
  std::vector<std::pair<unsigned, unsigned>> Uses, Defs; // (id, point)
  std::vector<std::pair<unsigned, unsigned>> CandVP;     // (point, vreg id)
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
        Defs.emplace_back((unsigned)Id, P);
      if (MO.readsReg())
        Uses.emplace_back((unsigned)Id, P);
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
          CandVP.emplace_back(P, VId[R]);
        ++CandId;
      }
    }
  }

  if (IdToV.empty())
    return;

  // Identify values that cross the block boundary; pin them to a register there
  // so any spill is balanced (stored AND reloaded) inside this block.
  SlotIndex BBStart = LIS.getInstructionIndex(*Pts.front()).getBaseIndex();
  SlotIndex BBEnd = LIS.getInstructionIndex(*Pts.back()).getRegSlot();
  std::vector<bool> LiveIn(IdToV.size(), false), LiveOut(IdToV.size(), false);
  for (unsigned Id = 0; Id < IdToV.size(); ++Id) {
    const LiveInterval &LI = LIS.getInterval(IdToV[Id]);
    auto &LR = LiveRange[Id];
    if (LI.liveAt(BBStart)) {
      LR.first = 0;
      LiveIn[Id] = true;
    }
    if (LI.liveAt(BBEnd)) {
      LR.second = (int)N - 1;
      LiveOut[Id] = true;
    }
  }

  // Static facts (registers + costs), shared by every window.
  std::vector<bool> IsGPRC(Regs.size(), false);
  std::ostringstream RF;
  for (unsigned I = 0; I < Regs.size(); ++I) {
    RF << "reg(" << I << ").";
    if (RISCV::GPRCRegClass.contains(Regs[I])) {
      RF << " gprc(" << I << ").";
      IsGPRC[I] = true;
    }
    RF << "\n";
  }
  RF << "storecost(4). reloadcost(4). compsave(2).\n";
  std::string RegFacts = RF.str();

  unsigned Wmax = std::max(2u, (unsigned)ASPSplitWindow);
  unsigned Wmin = std::max(2u, (unsigned)ASPSplitWindowMin);
  DenseMap<std::pair<unsigned, unsigned>, unsigned> RegAt; // (id,point) -> regid
  DenseMap<unsigned, int> ExitReg; // id -> regid at prev window's last point (-1=mem)
  bool AnyWindowSolved = false;

  // Build and solve the window [Lo, Lo+Width-1] against the current ExitReg
  // frozen boundary; fills Sol and Optimal, returns whether a model was found.
  auto solveWin = [&](unsigned Lo, unsigned Width, std::vector<Loc> &Sol,
                      bool &Optimal) -> bool {
    unsigned Hi = std::min(Lo + Width - 1, N - 1);
    std::ostringstream F;
    F << "point(" << Lo << ".." << Hi << ").\nfirst(" << Lo << ").\n" << RegFacts;
    for (unsigned Id = 0; Id < IdToV.size(); ++Id) {
      auto &LR = LiveRange[Id];
      int A = LR.first, B = LR.second;
      if (A > (int)Hi || B < (int)Lo)
        continue;
      int WLo = std::max(A, (int)Lo), WHi = std::min(B, (int)Hi);
      F << "live(" << Id << "," << WLo << ".." << WHi << ").\n";
      if (LiveOut[Id])
        for (int P = WLo; P <= WHi; ++P)
          F << "pin(" << Id << "," << P << ").\n";
      else if (LiveIn[Id] && A == 0 && Lo == 0)
        F << "pin(" << Id << ",0).\n";
      if (A < (int)Lo && !LiveOut[Id]) {
        auto It = ExitReg.find(Id);
        if (It != ExitReg.end())
          F << (It->second >= 0 ? "frozen_reg(" + std::to_string(Id) + "," +
                                      std::to_string(It->second) + ").\n"
                                : "frozen_mem(" + std::to_string(Id) + ").\n");
      }
    }
    for (auto &U : Uses)
      if (U.second >= Lo && U.second <= Hi)
        F << "use(" << U.first << "," << U.second << ").\n";
    for (auto &D : Defs)
      if (D.second >= Lo && D.second <= Hi)
        F << "def(" << D.first << "," << D.second << ").\n";
    unsigned C = 0;
    for (auto &PV : CandVP)
      if (PV.first >= Lo && PV.first <= Hi)
        F << "cand(c" << (C++) << "," << PV.second << "," << PV.first << ").\n";
    std::string Program = std::string(kSplitModel) + F.str();
    return runSplitSolve(Program, ASPSplitTimeLimit, Sol, Optimal);
  };

  // Adaptive ("binary search") window sizing: from each position find the
  // LARGEST window in [Wmin, Wmax] clingo proves optimal within the budget,
  // seeded at the previous window's width.  If the seed proves optimal, search
  // UPWARD (re-widen); else search DOWNWARD (narrow).  The Wmax ceiling makes
  // re-widening instant when pressure eases.  (Mirrors bisect_decompose.py.)
  unsigned Seed = Wmax;
  for (unsigned Lo = 0; Lo < N;) {
    unsigned Cap = std::min(Wmax, (unsigned)(N - Lo));
    unsigned S = std::max(Wmin, std::min(Seed, Cap));
    std::vector<Loc> SeedSol, BestSol;
    bool SeedOpt = false;
    bool SeedFound = solveWin(Lo, S, SeedSol, SeedOpt);
    unsigned BestW = 0; // largest width proven optimal (0 = none yet)
    if (SeedOpt) {
      BestSol = SeedSol;
      BestW = S;
      for (unsigned A = S + 1, B = Cap; A <= B;) { // re-widen
        unsigned M = (A + B) / 2;
        std::vector<Loc> Sc;
        bool O = false;
        solveWin(Lo, M, Sc, O);
        if (O) { BestSol = std::move(Sc); BestW = M; A = M + 1; }
        else { if (M == 0) break; B = M - 1; }
      }
    } else if (S > Wmin) {
      for (unsigned A = Wmin, B = S - 1; A <= B;) { // narrow
        unsigned M = (A + B) / 2;
        std::vector<Loc> Sc;
        bool O = false;
        solveWin(Lo, M, Sc, O);
        if (O) { BestSol = std::move(Sc); BestW = M; A = M + 1; }
        else { if (M == 0) break; B = M - 1; }
      }
    }

    unsigned ChosenW;
    std::vector<Loc> ChosenSol;
    bool Found;
    if (BestW != 0) { // got a proven-optimal window
      ChosenW = BestW;
      ChosenSol = std::move(BestSol);
      Found = true;
    } else if (SeedFound && S == Wmin) { // seed already minimal
      ChosenW = S;
      ChosenSol = std::move(SeedSol);
      Found = true;
    } else { // nothing optimal: fall back to the smallest window, best-effort
      ChosenW = Wmin;
      bool O = false;
      Found = solveWin(Lo, Wmin, ChosenSol, O);
    }

    unsigned Hi = std::min(Lo + ChosenW - 1, N - 1);
    if (Found) {
      AnyWindowSolved = true;
      for (const Loc &L : ChosenSol)
        RegAt[{L.V, L.P}] = L.R;
    }
    ExitReg.clear();
    for (unsigned Id = 0; Id < IdToV.size(); ++Id) {
      auto &LR = LiveRange[Id];
      if (LR.first <= (int)Hi && LR.second >= (int)Hi) {
        auto It = RegAt.find({Id, Hi});
        ExitReg[Id] = It != RegAt.end() ? (int)It->second : -1;
      }
    }
    Seed = ChosenW;
    Lo = Hi + 1;
  }
  if (!AnyWindowSolved)
    return;
  ++NumBlocksSolved;

  auto inReg = [&](unsigned Id, int P) {
    return RegAt.count({Id, (unsigned)P}) != 0;
  };

  // Record store/reload transition points per value (relative to this block).
  BlockPlan BP;
  unsigned LocalSplit = 0;
  for (unsigned Id = 0; Id < IdToV.size(); ++Id) {
    if (LiveOut[Id])
      continue; // never split (see pin emission)
    auto &LR = LiveRange[Id];
    VSplit VS;
    VS.V = IdToV[Id];
    for (int P = LR.first + 1; P <= LR.second; ++P) {
      bool Prev = inReg(Id, P - 1), Cur = inReg(Id, P);
      if (Prev && !Cur)
        VS.StorePts.push_back((unsigned)P);
      else if (!Prev && Cur)
        VS.ReloadPts.push_back((unsigned)P);
    }
    if (!VS.StorePts.empty() || !VS.ReloadPts.empty()) {
      BP.Splits.push_back(std::move(VS));
      ++LocalSplit;
      ++NumValuesSplit;
    }
  }
  if (!BP.Splits.empty()) {
    BP.Pts.assign(Pts.begin(), Pts.end());
    Plans.push_back(std::move(BP));
  }

  // Mark values placed in a GPRC register at a candidate point: constrain them
  // to GPRC so greedy realizes the compression.
  for (auto &PV : CandVP) {
    auto It = RegAt.find({PV.second, PV.first});
    if (It != RegAt.end() && It->second < IsGPRC.size() && IsGPRC[It->second]) {
      GPRCWanted.insert(IdToV[PV.second]);
      ++NumRealized;
    }
  }

  LLVM_DEBUG(dbgs() << "  [asp-split] " << MBB.getParent()->getName() << ":"
                    << MBB.getName() << " points=" << N
                    << " vregs=" << IdToV.size() << " cands=" << CandId
                    << " split=" << LocalSplit << "\n");
}

// Materialize the recorded split plans (SSA-preserving): for each value, walk
// the block in order, store the current vreg at each reg->mem point, reload into
// a FRESH vreg at each mem->reg point, and rewrite that value's operands in the
// remaining instructions to the current vreg.  This gives the value a holed live
// range (dead across the spilled gaps) without breaking SSA.  Also constrain the
// marked values to GPRC.  Liveness is recomputed afterwards (not preserved).
bool RISCVASPSplit::applyActions(MachineFunction &MF, const RISCVSubtarget &ST) {
  if (Plans.empty() && GPRCWanted.empty())
    return false;
  MachineRegisterInfo &MRI = MF.getRegInfo();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  const TargetInstrInfo *TII = ST.getInstrInfo();
  const TargetRegisterInfo *TRI = ST.getRegisterInfo();

  DenseMap<Register, int> Slot;
  auto getSlot = [&](Register V, const TargetRegisterClass *RC) -> int {
    auto It = Slot.find(V);
    if (It != Slot.end())
      return It->second;
    int FI = MFI.CreateSpillStackObject(TRI->getSpillSize(*RC),
                                        TRI->getSpillAlign(*RC));
    Slot[V] = FI;
    return FI;
  };

  bool Changed = false;
  for (BlockPlan &BP : Plans) {
    for (VSplit &VS : BP.Splits) {
      const TargetRegisterClass *RC = MRI.getRegClass(VS.V);
      int FI = getSlot(VS.V, RC);
      DenseSet<unsigned> StoreAt(VS.StorePts.begin(), VS.StorePts.end());
      DenseSet<unsigned> ReloadAt(VS.ReloadPts.begin(), VS.ReloadPts.end());
      Register Cur = VS.V;
      for (unsigned P = 0; P < BP.Pts.size(); ++P) {
        MachineInstr *MI = BP.Pts[P];
        MachineBasicBlock &MBB = *MI->getParent();
        MachineBasicBlock::iterator It = MI->getIterator();
        if (ReloadAt.count(P)) {
          Register NV = MRI.createVirtualRegister(RC);
          TII->loadRegFromStackSlot(MBB, It, NV, FI, RC, TRI, NV);
          Cur = NV;
        }
        if (StoreAt.count(P))
          TII->storeRegToStackSlot(MBB, It, Cur, /*isKill=*/false, FI, RC, TRI,
                                   Cur);
        if (Cur != VS.V)
          for (MachineOperand &MO : MI->operands())
            if (MO.isReg() && MO.getReg() == VS.V)
              MO.setReg(Cur);
      }
      Changed = true;
    }
  }
  for (Register V : GPRCWanted)
    if (MRI.constrainRegClass(V, &RISCV::GPRCRegClass))
      Changed = true;
  return Changed;
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
  Plans.clear();
  GPRCWanted.clear();
  LLVM_DEBUG(dbgs() << "[asp-split] function " << MF.getName() << "\n");
  // Phase A: solve every block against the original liveness, recording actions.
  for (MachineBasicBlock &MBB : MF)
    solveBlock(MBB, LIS, MRI, ST);
  // Phase B: materialize them (mutates the MIR; liveness recomputed for RA).
  return applyActions(MF, ST);
#else
  return false;
#endif
}

} // end anonymous namespace

INITIALIZE_PASS_BEGIN(RISCVASPSplit, DEBUG_TYPE,
                      "RISC-V decomposed ASP live-range splitting", false, false)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_END(RISCVASPSplit, DEBUG_TYPE,
                    "RISC-V decomposed ASP live-range splitting", false, false)

FunctionPass *llvm::createRISCVASPSplitPass() { return new RISCVASPSplit(); }
