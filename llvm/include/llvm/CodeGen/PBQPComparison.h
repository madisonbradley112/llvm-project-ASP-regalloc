//===- PBQPComparison.h ---- PBQP Allocator Comparison Framework ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides a comparison framework for validating alternative PBQP
// implementations against the reference graph-based implementation.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_PBQPCOMPARISON_H
#define LLVM_CODEGEN_PBQPCOMPARISON_H

#include "llvm/CodeGen/Register.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <string>
#include <vector>

namespace llvm {

/// Holds the allocation result for a single virtual register.
struct VRegAllocation {
  Register VReg;           // Virtual register ID
  unsigned PhysReg;        // Physical register ID (0 = spilled)
  std::string RegName;     // Human-readable register name
  double AllocationCost;   // Cost of this allocation
  bool IsSpilled;          // Whether this vreg was spilled

  VRegAllocation() : VReg(0), PhysReg(0), AllocationCost(0), IsSpilled(false) {}
  
  VRegAllocation(Register VReg, unsigned PhysReg, const std::string &RegName,
                 double Cost, bool Spilled)
      : VReg(VReg), PhysReg(PhysReg), RegName(RegName), AllocationCost(Cost),
        IsSpilled(Spilled) {}
};

/// Holds the complete allocation results for a function.
class AllocationResult {
public:
  std::string FunctionName;
  std::vector<VRegAllocation> Allocations;
  double TotalCost = 0.0;
  unsigned NumVRegs = 0;
  unsigned NumSpilled = 0;
  unsigned Round = 0;  // Which allocation round (for iterative allocators)

  /// Add an allocation result for a vreg.
  void addAllocation(Register VReg, unsigned PhysReg,
                     const std::string &RegName, double Cost, bool Spilled) {
    Allocations.emplace_back(VReg, PhysReg, RegName, Cost, Spilled);
    if (Spilled) {
      NumSpilled++;
    }
    TotalCost += Cost;
    NumVRegs++;
  }

  /// Clear all allocations.
  void clear() {
    Allocations.clear();
    TotalCost = 0.0;
    NumVRegs = 0;
    NumSpilled = 0;
  }

  /// Export allocation results to a simple text format.
  void exportToText(raw_ostream &OS) const;

  /// Export allocation results to JSON format.
  void exportToJSON(raw_ostream &OS) const;
};

/// Comparison results between two allocation implementations.
class ComparisonResults {
public:
  bool AllocationMatches = true;
  bool CostMatches = true;
  bool SpillMatches = true;
  
  std::vector<std::pair<Register, unsigned>> AllocationDifferences;
  double CostDifference = 0.0;
  std::vector<Register> SpillDifferences;

  /// Print detailed comparison results.
  void print(raw_ostream &OS) const;

  /// Returns true if all checks pass (implementations are equivalent).
  bool isEquivalent() const {
    return AllocationMatches && CostMatches && SpillMatches;
  }
};

/// Compare two allocation results with specified tolerance.
/// Returns comparison results with detailed differences.
ComparisonResults compareAllocations(const AllocationResult &Reference,
                                     const AllocationResult &Candidate,
                                     double CostTolerance = 1.0);

} // namespace llvm

#endif // LLVM_CODEGEN_PBQPCOMPARISON_H
