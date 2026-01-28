//===- PBQPComparison.cpp ---- PBQP Allocator Comparison Framework --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/PBQPComparison.h"
#include "llvm/Support/Format.h"
#include <cmath>

using namespace llvm;

void AllocationResult::exportToText(raw_ostream &OS) const {
  OS << "Function: " << FunctionName << "\n";
  OS << "Round: " << Round << "\n";
  OS << "Total Cost: " << format("%.2f", TotalCost) << "\n";
  OS << "VRegs: " << NumVRegs << ", Spilled: " << NumSpilled << "\n";
  OS << "---\n";

  for (const auto &Alloc : Allocations) {
    OS << "VReg" << Alloc.VReg << " -> ";
    if (Alloc.IsSpilled) {
      OS << "SPILLED";
    } else {
      OS << Alloc.RegName << " (" << Alloc.PhysReg << ")";
    }
    OS << " (cost: " << format("%.2f", Alloc.AllocationCost) << ")\n";
  }
}

void AllocationResult::exportToJSON(raw_ostream &OS) const {
  OS << "{\n";
  OS << "  \"function\": \"" << FunctionName << "\",\n";
  OS << "  \"round\": " << Round << ",\n";
  OS << "  \"total_cost\": " << format("%.2f", TotalCost) << ",\n";
  OS << "  \"num_vregs\": " << NumVRegs << ",\n";
  OS << "  \"num_spilled\": " << NumSpilled << ",\n";
  OS << "  \"allocations\": [\n";

  for (size_t i = 0; i < Allocations.size(); ++i) {
    const auto &Alloc = Allocations[i];
    OS << "    {\n";
    OS << "      \"vreg\": " << Alloc.VReg << ",\n";
    OS << "      \"physreg\": " << Alloc.PhysReg << ",\n";
    OS << "      \"reg_name\": \"" << Alloc.RegName << "\",\n";
    OS << "      \"cost\": " << format("%.2f", Alloc.AllocationCost) << ",\n";
    OS << "      \"spilled\": " << (Alloc.IsSpilled ? "true" : "false") << "\n";
    OS << "    }";
    if (i + 1 < Allocations.size()) {
      OS << ",";
    }
    OS << "\n";
  }

  OS << "  ]\n";
  OS << "}\n";
}

void ComparisonResults::print(raw_ostream &OS) const {
  OS << "=== Allocation Comparison Results ===\n";
  
  if (isEquivalent()) {
    OS << "✓ All checks PASSED - Implementations are equivalent\n";
    return;
  }

  OS << "\nCheck Results:\n";
  OS << (AllocationMatches ? "✓" : "✗") << " Allocation matches\n";
  OS << (CostMatches ? "✓" : "✗") << " Cost matches\n";
  OS << (SpillMatches ? "✓" : "✗") << " Spill decisions match\n";

  if (!AllocationMatches && !AllocationDifferences.empty()) {
    OS << "\nAllocation Differences:\n";
    for (const auto &Diff : AllocationDifferences) {
      OS << "  VReg " << Diff.first << ": expected " << Diff.second << "\n";
    }
  }

  if (!CostMatches) {
    OS << "\nCost Difference: " << format("%.2f", CostDifference) << "\n";
  }

  if (!SpillMatches && !SpillDifferences.empty()) {
    OS << "\nSpill Differences:\n";
    for (const auto &VReg : SpillDifferences) {
      OS << "  VReg " << VReg << "\n";
    }
  }
}

ComparisonResults compareAllocations(const AllocationResult &Reference,
                                     const AllocationResult &Candidate,
                                     double CostTolerance) {
  ComparisonResults Results;

  // Check if VReg allocations match
  if (Reference.Allocations.size() != Candidate.Allocations.size()) {
    Results.AllocationMatches = false;
  } else {
    for (size_t i = 0; i < Reference.Allocations.size(); ++i) {
      const auto &RefAlloc = Reference.Allocations[i];
      const auto &CandAlloc = Candidate.Allocations[i];

      // Compare VReg IDs
      if (RefAlloc.VReg != CandAlloc.VReg) {
        Results.AllocationMatches = false;
        Results.AllocationDifferences.push_back({RefAlloc.VReg, CandAlloc.PhysReg});
        continue;
      }

      // Compare physical register assignments
      if (RefAlloc.PhysReg != CandAlloc.PhysReg) {
        Results.AllocationMatches = false;
        Results.AllocationDifferences.push_back({RefAlloc.VReg, CandAlloc.PhysReg});
      }

      // Compare spill decisions
      if (RefAlloc.IsSpilled != CandAlloc.IsSpilled) {
        Results.SpillMatches = false;
        Results.SpillDifferences.push_back(RefAlloc.VReg);
      }
    }
  }

  // Check if costs match (within tolerance)
  double Diff = std::abs(Reference.TotalCost - Candidate.TotalCost);
  if (Diff > CostTolerance) {
    Results.CostMatches = false;
    Results.CostDifference = Diff;
  }

  return Results;
}
