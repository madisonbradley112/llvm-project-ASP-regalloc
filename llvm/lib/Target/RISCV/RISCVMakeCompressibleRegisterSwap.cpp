//===-- RISCVMakeCompressibleRegisterSwap.cpp ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass performs live-range splitting with the goal of increasing the
// amount of compressed instructions, thereby reducing code size.
//
// It searches for lost compressibility at function call boundaries. The
// compressible register set is s0, s1, a0-a5, but only s0 and s1 are
// callee-saved; the rest are clobbered across calls. So a value that must
// survive a call has twelve callee-saved registers to choose from and only two
// that keep it compressible, and everything it touches afterwards stays 4 bytes
// wide.
//
// Splitting recovers some of that: after the call, copy the value into a fresh
// virtual register restricted to GPRC and rewrite the uses that copy dominates.
// Being pre-register-allocation, the class constraint alone forces the
// allocator's hand; this pass never picks a physical register.
//
// Three phases, in this order:
//   1) Collect every use a copy placed immediately after the call would serve.
//      The just-after-call position is the highest legal one, so the count
//      measured there is an upper bound over all placements.
//   2) Decide profitability from that count. The decision is final.
//   3) Place the copy at the nearest common dominator of the collected uses,
//      then hoist out of loops. Neither step can change the count.
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVSubtarget.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "riscv-make-compressible-register-swap"
#define PASS_NAME "RISC-V Make Compressible Register Swap"

STATISTIC(NumCandidates, "Candidates considered");
STATISTIC(NumSplits, "Call-boundary splits inserted");
STATISTIC(NumFixed, "Instructions predicted to become compressible");
STATISTIC(NumRejectedCost, "Candidates rejected by the cost model");
STATISTIC(NumRejectedCall, "Uses rejected by the next-call ceiling");
STATISTIC(NumSunk, "Copies sunk below the call");
STATISTIC(NumHoisted, "Copies hoisted out of a loop");

static cl::opt<bool> EnableSwap("riscv-swap-enable", cl::Hidden, cl::init(true),
                                cl::desc("Enable compression-aware live range "
                                         "splitting at call boundaries"));

static cl::opt<unsigned>
    MinBenefit("riscv-swap-min-benefit", cl::Hidden, cl::init(2),
               cl::desc("Minimum instructions made compressible to justify "
                        "one c.mv"));

static cl::opt<bool> StopAtNextCall(
    "riscv-swap-stop-at-next-call", cl::Hidden, cl::init(true),
    cl::desc("Do not collect uses past the next call. Policy, not "
             "correctness: a range crossing a call is legal, it just forces "
             "the allocator onto s0/s1 or into a spill."));

static cl::opt<bool>
    EnableSinking("riscv-swap-sink", cl::Hidden, cl::init(true),
                  cl::desc("Sink the copy to the nearest common dominator of "
                           "its uses and hoist it out of loops"));

static cl::opt<unsigned>
    MaxLoopDepth("riscv-swap-max-loop-depth", cl::Hidden, cl::init(~0u),
                 cl::desc("Reject candidates whose final insertion point is "
                          "nested deeper than this"));

namespace {

/// A repatriation opportunity: copy \p Reg into a fresh GPRC vreg somewhere
/// after \p Call, and rewrite \p Uses to read the new register.
struct SplitCandidate {
  Register Reg;
  MachineInstr *Call = nullptr;
  MachineInstr *InsertPt = nullptr;
  SlotIndex SplitPoint;
  SmallVector<MachineOperand *, 8> Uses;
  unsigned Benefit = 0;
};

class RISCVMakeCompressibleRegisterSwap : public MachineFunctionPass {
public:
  static char ID;

  RISCVMakeCompressibleRegisterSwap() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return PASS_NAME; }

  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool runOnMachineFunction(MachineFunction &Fn) override;

private:
  MachineRegisterInfo *MRI = nullptr;
  LiveIntervals *LIS = nullptr;
  MachineDominatorTree *MDT = nullptr;
  MachineLoopInfo *MLI = nullptr;
  const RISCVInstrInfo *TII = nullptr;
  const RISCVSubtarget *STI = nullptr;

  void collectCandidates(SmallVectorImpl<SplitCandidate> &Out);
  bool buildCandidate(SplitCandidate &C, const LiveInterval &LI,
                      ArrayRef<SlotIndex> RegMaskSlots);
  bool performSplit(SplitCandidate &C);
  MachineBasicBlock::iterator chooseInsertPoint(const SplitCandidate &C);

  bool isSplittableCall(const MachineInstr &MI) const;
  bool mustStayAdjacentToCall(const MachineInstr &MI) const;
  bool mayBeGPRC(Register R) const;
  bool wouldCompressIfGPRC(const MachineInstr &MI, unsigned OpNo) const;
};

} // end anonymous namespace

char RISCVMakeCompressibleRegisterSwap::ID = 0;

INITIALIZE_PASS_BEGIN(RISCVMakeCompressibleRegisterSwap, DEBUG_TYPE, PASS_NAME,
                      false, false)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_END(RISCVMakeCompressibleRegisterSwap, DEBUG_TYPE, PASS_NAME,
                    false, false)

void RISCVMakeCompressibleRegisterSwap::getAnalysisUsage(
    AnalysisUsage &AU) const {
  AU.addRequired<LiveIntervalsWrapperPass>();
  AU.addPreserved<LiveIntervalsWrapperPass>();
  AU.addRequired<MachineDominatorTreeWrapperPass>();
  AU.addPreserved<MachineDominatorTreeWrapperPass>();
  AU.addRequired<MachineLoopInfoWrapperPass>();
  AU.addPreserved<MachineLoopInfoWrapperPass>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

//===----------------------------------------------------------------------===//
// Small predicates
//===----------------------------------------------------------------------===//

bool RISCVMakeCompressibleRegisterSwap::isSplittableCall(
    const MachineInstr &MI) const {
  // getRegMaskSlots() is mostly, but not only, calls.
  if (!MI.isCall())
    return false;
  if (MI.isReturn())     // tail call: nothing is live afterwards
    return false;
  if (MI.isTerminator()) // invoke: no "after the call" in this block
    return false;
  return true;
}

bool RISCVMakeCompressibleRegisterSwap::mustStayAdjacentToCall(
    const MachineInstr &MI) const {
  if (MI.isEHLabel() || MI.isCFIInstruction() || MI.isDebugInstr())
    return true;
  if (MI.getOpcode() == TII->getCallFrameDestroyOpcode())
    return true;
  // Copies pulling the return value out of $x10/$x11.
  if (MI.isCopy() && MI.getOperand(1).isReg() &&
      MI.getOperand(1).getReg().isPhysical())
    return true;
  return false;
}

/// Could \p R plausibly end up in x8-x15?
///
/// Pre-RA this is unknowable for an unconstrained virtual register, so we
/// assume optimistically that it will. The prior is defensible: a0-a5 sit early
/// in the allocation order and values not live across calls usually do get
/// them, which is exactly why measured blockage concentrates in s2-s11. But it
/// makes Benefit an upper bound -- the gap between NumFixed and a post-link
/// count of what actually became 16-bit is the over-estimate.
bool RISCVMakeCompressibleRegisterSwap::mayBeGPRC(Register R) const {
  if (!R)
    return false;
  if (R.isVirtual()) {
    const TargetRegisterClass *RC = MRI->getRegClass(R);
    return RC == &RISCV::GPRRegClass || RISCV::GPRCRegClass.hasSubClassEq(RC);
  }
  return RISCV::GPRCRegClass.contains(R);
}

/// Would \p MI encode in 16 bits if the register operand at \p OpNo were in
/// GPRC, holding the opcode, immediate and operand structure fixed?
///
/// Mirrors the CompressPat records in RISCVInstrInfoC.td. Each pattern encodes
/// three constraints at once: register class, immediate predicate, and (via
/// repeated operand names) the two-address requirement.
bool RISCVMakeCompressibleRegisterSwap::wouldCompressIfGPRC(
    const MachineInstr &MI, unsigned OpNo) const {
  const bool IsRV64 = STI->is64Bit();

  auto reg = [&](unsigned N) -> Register {
    const MachineOperand &MO = MI.getOperand(N);
    return MO.isReg() ? MO.getReg() : Register();
  };
  // Every *other* register the compressed form needs in GPRC must plausibly
  // get there; the one we were asked about is the one we are going to fix.
  auto otherOk = [&](unsigned N) { return N == OpNo || mayBeGPRC(reg(N)); };

  switch (MI.getOpcode()) {
  default:
    return false;

  // CL / CS: loads and stores. Data register and base must both be in GPRC and
  // the offset must be scaled and in range. SP-based forms (C_LWSP etc.) take
  // any register and a wider offset, so an SP base means nothing to gain here.
  case RISCV::LW:
  case RISCV::SW: {
    if (reg(1) == RISCV::X2 || !MI.getOperand(2).isImm())
      return false;
    if (!isShiftedUInt<5, 2>(MI.getOperand(2).getImm())) // uimm7_lsb00
      return false;
    return reg(0) && reg(1) && otherOk(0) && otherOk(1);
  }
  case RISCV::LD:
  case RISCV::SD: {
    if (!IsRV64)
      return false;
    if (reg(1) == RISCV::X2 || !MI.getOperand(2).isImm())
      return false;
    if (!isShiftedUInt<5, 3>(MI.getOperand(2).getImm())) // uimm8_lsb000
      return false;
    return reg(0) && reg(1) && otherOk(0) && otherOk(1);
  }

  // CB: andi / srli / srai. Two-address, rd must equal rs1.
  case RISCV::ANDI: {
    if (reg(0) != reg(1) || !MI.getOperand(2).isImm())
      return false;
    if (!isInt<6>(MI.getOperand(2).getImm())) // simm6
      return false;
    return otherOk(0) && otherOk(1);
  }
  case RISCV::SRLI:
  case RISCV::SRAI: {
    if (reg(0) != reg(1) || !MI.getOperand(2).isImm())
      return false;
    int64_t Sh = MI.getOperand(2).getImm();
    if (Sh == 0 || Sh >= (IsRV64 ? 64 : 32)) // uimmlog2xlennonzero
      return false;
    return otherOk(0) && otherOk(1);
  }

  // CA: two-address ALU. rd must alias one source. Commuted patterns exist for
  // AND/OR/XOR/ADDW (isCompressOnly in the .td) but NOT for SUB/SUBW.
  case RISCV::AND:
  case RISCV::OR:
  case RISCV::XOR:
    if (reg(0) != reg(1) && reg(0) != reg(2))
      return false;
    return otherOk(0) && otherOk(1) && otherOk(2);
  case RISCV::SUB:
    if (reg(0) != reg(1))
      return false;
    return otherOk(0) && otherOk(1) && otherOk(2);
  case RISCV::ADDW:
    if (!IsRV64)
      return false;
    if (reg(0) != reg(1) && reg(0) != reg(2))
      return false;
    return otherOk(0) && otherOk(1) && otherOk(2);
  case RISCV::SUBW:
    if (!IsRV64)
      return false;
    if (reg(0) != reg(1))
      return false;
    return otherOk(0) && otherOk(1) && otherOk(2);

  // C_BEQZ / C_BNEZ are deliberately omitted: the displacement is not known
  // until BranchRelaxation, so including them adds a class of estimation error
  // for a small share of the population.
  }
}

//===----------------------------------------------------------------------===//
// Phase 1: collection
//===----------------------------------------------------------------------===//

void RISCVMakeCompressibleRegisterSwap::collectCandidates(
    SmallVectorImpl<SplitCandidate> &Out) {

  ArrayRef<SlotIndex> RegMaskSlots = LIS->getRegMaskSlots();
  if (RegMaskSlots.empty())
    return; // no calls, nothing to do

  // Invert the loop: walk intervals once and binary-search the sorted regmask
  // index, rather than testing every vreg at every call.
  for (unsigned I = 0, E = MRI->getNumVirtRegs(); I != E; ++I) {
    Register Reg = Register::index2VirtReg(I);
    if (MRI->reg_nodbg_empty(Reg) || !LIS->hasInterval(Reg))
      continue;
    if (MRI->getRegClass(Reg) != &RISCV::GPRRegClass)
      continue; // phase 1: plain integer vregs only

    const LiveInterval &LI = LIS->getInterval(Reg);

    for (const LiveInterval::Segment &S : LI) {
      for (auto It = llvm::lower_bound(RegMaskSlots, S.start);
           It != RegMaskSlots.end() && *It < S.end; ++It) {
        MachineInstr *Call = LIS->getInstructionFromIndex(*It);
        if (!Call || !isSplittableCall(*Call))
          continue;

        SplitCandidate C;
        C.Reg = Reg;
        C.Call = Call;
        if (!buildCandidate(C, LI, RegMaskSlots))
          continue;
        ++NumCandidates;
        if (C.Benefit < MinBenefit) {
          ++NumRejectedCost;
          continue;
        }
        Out.push_back(std::move(C));
      }
    }
  }
}

bool RISCVMakeCompressibleRegisterSwap::buildCandidate(
    SplitCandidate &C, const LiveInterval &LI,
    ArrayRef<SlotIndex> RegMaskSlots) {

  // The highest legal insertion point, for maximal dominance. Collecting here
  // makes Benefit an upper bound over all placements, so the reject decision is
  // safe and final -- no lower position can rescue a candidate that fails.
  MachineBasicBlock &MBB = *C.Call->getParent();
  MachineBasicBlock::iterator It = std::next(C.Call->getIterator());
  while (It != MBB.end() && mustStayAdjacentToCall(*It))
    ++It;
  if (It == MBB.end())
    return false;
  C.InsertPt = &*It;
  C.SplitPoint = LIS->getInstructionIndex(*C.InsertPt).getBaseIndex();

  // A null VNInfo here *is* the "not live across the call" test.
  VNInfo *VNAtSplit = LI.getVNInfoAt(C.SplitPoint);
  if (!VNAtSplit)
    return false;

  SlotIndex CallLimit;
  if (StopAtNextCall) {
    auto NextCall = llvm::upper_bound(RegMaskSlots, C.SplitPoint);
    if (NextCall != RegMaskSlots.end())
      CallLimit = *NextCall;
  }

  // Benefit counts *instructions*; Uses collects *operands*. They differ when
  // one instruction reads the parent twice, e.g. SW %1, 0(%1): two rewrites,
  // still only two bytes saved.
  SmallPtrSet<const MachineInstr *, 8> Counted;

  for (MachineOperand &MO : MRI->use_nodbg_operands(C.Reg)) {
    MachineInstr *UseMI = MO.getParent();

    // A tied def+use would make this instruction define the new vreg, leaving
    // every later reader of the parent stale. Phase 1 skips them.
    if (MO.isDef() || MO.isTied())
      continue;
    if (!wouldCompressIfGPRC(*UseMI, MO.getOperandNo()))
      continue;

    SlotIndex UseIdx = LIS->getInstructionIndex(*UseMI).getRegSlot();
    if (CallLimit.isValid() && UseIdx >= CallLimit) {
      ++NumRejectedCall;
      continue;
    }
    if (LI.getVNInfoAt(UseIdx) != VNAtSplit)
      continue; // a different definition reaches here
    if (!MDT->dominates(C.InsertPt, UseMI))
      continue; // priciest test (linear within a block), so last

    C.Uses.push_back(&MO);
    Counted.insert(UseMI);
  }

  C.Benefit = Counted.size();
  return !C.Uses.empty();
}

//===----------------------------------------------------------------------===//
// Phase 3: placement and rewrite
//===----------------------------------------------------------------------===//

/// Sink the copy to the nearest common dominator of the collected uses, then
/// hoist out of any enclosing loop. An NCD dominates every block it was
/// computed from, so Benefit is unchanged and both moves are free.
MachineBasicBlock::iterator
RISCVMakeCompressibleRegisterSwap::chooseInsertPoint(const SplitCandidate &C) {
  MachineBasicBlock::iterator Fallback = C.InsertPt->getIterator();
  if (!EnableSinking)
    return Fallback;

  MachineBasicBlock *CallMBB = C.InsertPt->getParent();
  MachineBasicBlock *Sink = C.Uses.front()->getParent()->getParent();
  for (MachineOperand *MO : C.Uses) {
    Sink = MDT->findNearestCommonDominator(Sink, MO->getParent()->getParent());
    if (!Sink)
      return Fallback;
  }

  // Hoist out of loops while the result still lies after the call.
  while (MachineLoop *L = MLI->getLoopFor(Sink)) {
    MachineBasicBlock *PH = L->getLoopPreheader();
    if (!PH || PH == Sink || PH == CallMBB)
      break;
    MachineBasicBlock::iterator PHI = PH->SkipPHIsLabelsAndDebug(PH->begin());
    if (PHI == PH->end() || !MDT->dominates(C.InsertPt, &*PHI))
      break;
    Sink = PH;
    ++NumHoisted;
  }

  if (Sink == CallMBB)
    return Fallback; // cannot go earlier than the call itself

  MachineBasicBlock::iterator SinkIt = Sink->SkipPHIsLabelsAndDebug(Sink->begin());
  if (SinkIt == Sink->end() || !MDT->dominates(C.InsertPt, &*SinkIt))
    return Fallback;

  ++NumSunk;
  return SinkIt;
}

bool RISCVMakeCompressibleRegisterSwap::performSplit(SplitCandidate &C) {
  MachineBasicBlock::iterator InsertIt = chooseInsertPoint(C);
  MachineBasicBlock &MBB = *InsertIt->getParent();

  if (MaxLoopDepth != ~0u && MLI->getLoopDepth(&MBB) > MaxLoopDepth) {
    ++NumRejectedCost;
    return false;
  }

  Register New = MRI->createVirtualRegister(&RISCV::GPRCRegClass);
  MachineInstr *Copy = BuildMI(MBB, InsertIt, C.Call->getDebugLoc(),
                               TII->get(TargetOpcode::COPY), New)
                           .addReg(C.Reg);

  // Guard: with -riscv-swap-stop-at-next-call=false, candidates from the same
  // parent can overlap, and an earlier split may already have rewritten this
  // operand. Re-check rather than clobbering a previous repatriation.
  unsigned Rewritten = 0;
  for (MachineOperand *MO : C.Uses) {
    if (MO->getReg() != C.Reg)
      continue;
    MO->setReg(New);
    ++Rewritten;
  }
  if (Rewritten == 0) {
    Copy->eraseFromParent();
    return false;
  }

  LIS->InsertMachineInstrInMaps(*Copy);
  LIS->createAndComputeVirtRegInterval(New);
  LIS->removeInterval(C.Reg);
  LIS->createAndComputeVirtRegInterval(C.Reg);

  LLVM_DEBUG(dbgs() << "  split %" << C.Reg.virtRegIndex()
                    << " -> %" << New.virtRegIndex() << "  benefit "
                    << C.Benefit << ", " << Rewritten
                    << " operands rewritten\n");

  ++NumSplits;
  NumFixed += C.Benefit;
  return true;
}

//===----------------------------------------------------------------------===//
// Entry point
//===----------------------------------------------------------------------===//

bool RISCVMakeCompressibleRegisterSwap::runOnMachineFunction(
    MachineFunction &Fn) {
  if (!EnableSwap)
    return false;

  // This is a size optimization. hasOptSize() covers -Os and -Oz.
  if (skipFunction(Fn.getFunction()) || !Fn.getFunction().hasOptSize())
    return false;

  STI = &Fn.getSubtarget<RISCVSubtarget>();
  if (!STI->hasStdExtZca())
    return false;

  MRI = &Fn.getRegInfo();
  TII = STI->getInstrInfo();
  LIS = &getAnalysis<LiveIntervalsWrapperPass>().getLIS();
  MDT = &getAnalysis<MachineDominatorTreeWrapperPass>().getDomTree();
  MLI = &getAnalysis<MachineLoopInfoWrapperPass>().getLI();

  LLVM_DEBUG(dbgs() << "*** " << PASS_NAME << ": " << Fn.getName() << " ***\n");

  SmallVector<SplitCandidate, 16> Candidates;
  collectCandidates(Candidates);

  bool Changed = false;
  for (SplitCandidate &C : Candidates)
    Changed |= performSplit(C);

  return Changed;
}

/// Returns an instance of the Make Compressible Register Swap pass.
FunctionPass *llvm::createRISCVMakeCompressibleRegisterSwapPass() {
  return new RISCVMakeCompressibleRegisterSwap();
}
