//===- RegAllocPBQPASP.h ----- ASP-based PBQP solver -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Solves a fully-constructed PBQP register-allocation graph by encoding it as
// Answer Set Programming and dispatching to Clingo via its C API. The encoding
// is generic PBQP (node/option/cost/forbidden facts plus a choice rule and
// #minimize), so all register-allocation specifics (AllowedRegs domain,
// callee-saved preference, interference via TRI.regsOverlap, coalescing
// weighted by block frequency) come from the graph LLVM has already built.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_REGALLOCPBQPASP_H
#define LLVM_CODEGEN_REGALLOCPBQPASP_H

#include "llvm/CodeGen/RegAllocPBQP.h"

namespace llvm {
namespace PBQP {
namespace RegAlloc {

/// Solve a PBQP register-allocation graph using Clingo (Answer Set
/// Programming). Returns a Solution selecting one option per node.
///
/// Floating-point costs are scaled by 1000 and rounded to integers (Clingo
/// is integer-only). Infinity costs are encoded as forbidden-pair
/// constraints rather than as huge integers, keeping the search clean.
///
/// Aborts with report_fatal_error if the encoding is UNSAT or Clingo fails;
/// such cases indicate a malformed graph (no valid allocation exists).
Solution aspSolve(PBQPRAGraph &G);

} // namespace RegAlloc
} // namespace PBQP
} // namespace llvm

#endif // LLVM_CODEGEN_REGALLOCPBQPASP_H
