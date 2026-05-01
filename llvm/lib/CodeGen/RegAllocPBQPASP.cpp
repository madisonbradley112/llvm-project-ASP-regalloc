//===- RegAllocPBQPASP.cpp ----- ASP-based PBQP solver --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/RegAllocPBQPASP.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#ifdef LLVM_PBQP_HAVE_CLINGO
#include <clingo.h>
#endif

#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#define DEBUG_TYPE "pbqp-asp"

using namespace llvm;
using namespace llvm::PBQP;
using namespace llvm::PBQP::RegAlloc;

namespace {

/// Multiplier applied to floating-point costs before rounding to int. Clingo
/// is integer-only; 1000x preserves three decimal digits, well within the
/// resolution of typical LiveInterval weights and block-frequency benefits.
constexpr int CostScale = 1000;

inline bool isInfiniteCost(PBQPNum C) {
  return C == std::numeric_limits<PBQPNum>::infinity();
}

inline long scaleCost(PBQPNum C) {
  return static_cast<long>(std::lround(C * CostScale));
}

/// The static portion of the ASP program: choice rule, forbidden-pair
/// constraints, and the optimization objective. Dynamic facts (node/option/
/// cost) are appended per-graph.
const char *const kPBQPProgramPrelude =
    "% Generic PBQP-as-ASP encoding.\n"
    "% Each node selects exactly one option.\n"
    "1 { select(N, O) : option(N, O) } 1 :- node(N).\n"
    "\n"
    "% Hard constraints from infinity-cost entries.\n"
    ":- select(N, O), forbidden_node(N, O).\n"
    ":- select(N1, I), select(N2, J), forbidden_edge(N1, N2, I, J).\n"
    "\n"
    "% Minimize the sum of selected node costs and edge costs.\n"
    "% The trailing tuple terms are uniqueness keys (Clingo's #minimize uses\n"
    "% set semantics on the full tuple; without distinct keys, identical\n"
    "% weights from different (N,O) or (N1,N2,I,J) would collapse into one).\n"
    "#minimize {\n"
    "  C, node, N, O          : select(N, O), node_cost(N, O, C) ;\n"
    "  C, edge, N1, N2, I, J  : select(N1, I), select(N2, J),\n"
    "                            edge_cost(N1, N2, I, J, C)\n"
    "}.\n"
    "\n"
    "#show select/2.\n";

/// Translate the PBQP graph into ASP facts. Costs are scaled by CostScale;
/// infinity costs become forbidden_node/forbidden_edge atoms.
std::string encodeGraph(const PBQPRAGraph &G) {
  std::ostringstream OS;
  OS << kPBQPProgramPrelude << "\n";

  // Nodes and their per-option costs.
  for (auto NId : G.nodeIds()) {
    OS << "node(" << NId << ").\n";
    const auto &NodeCosts = G.getNodeCosts(NId);
    for (unsigned I = 0, E = NodeCosts.getLength(); I != E; ++I) {
      OS << "option(" << NId << "," << I << ").\n";
      PBQPNum C = NodeCosts[I];
      if (isInfiniteCost(C)) {
        OS << "forbidden_node(" << NId << "," << I << ").\n";
      } else {
        // Emit even zero costs so #minimize sees every selected option in
        // its sum (otherwise total_cost loses node contributions). Cheap.
        OS << "node_cost(" << NId << "," << I << "," << scaleCost(C) << ").\n";
      }
    }
  }

  // Edges and their pairwise cost matrices.
  for (auto EId : G.edgeIds()) {
    auto N1Id = G.getEdgeNode1Id(EId);
    auto N2Id = G.getEdgeNode2Id(EId);
    const auto &EdgeCosts = G.getEdgeCosts(EId);
    for (unsigned I = 0, RE = EdgeCosts.getRows(); I != RE; ++I) {
      for (unsigned J = 0, CE = EdgeCosts.getCols(); J != CE; ++J) {
        PBQPNum C = EdgeCosts[I][J];
        if (isInfiniteCost(C)) {
          OS << "forbidden_edge(" << N1Id << "," << N2Id << "," << I << ","
             << J << ").\n";
        } else if (C != 0) {
          // Skip zero edge entries; they don't affect the sum and most of
          // a typical PBQP edge matrix is zero.
          OS << "edge_cost(" << N1Id << "," << N2Id << "," << I << "," << J
             << "," << scaleCost(C) << ").\n";
        }
      }
    }
  }

  return OS.str();
}

#ifdef LLVM_PBQP_HAVE_CLINGO

/// Extract (NodeId, OptionIdx) from a `select(N, O)` Clingo symbol. Returns
/// false if the symbol isn't a well-formed select fact.
bool parseSelectAtom(clingo_symbol_t Sym, unsigned &NodeId,
                     unsigned &OptIdx) {
  if (clingo_symbol_type(Sym) != clingo_symbol_type_function)
    return false;

  char const *Name = nullptr;
  if (!clingo_symbol_name(Sym, &Name) || std::strcmp(Name, "select") != 0)
    return false;

  clingo_symbol_t const *Args = nullptr;
  size_t NumArgs = 0;
  if (!clingo_symbol_arguments(Sym, &Args, &NumArgs) || NumArgs != 2)
    return false;

  int N = 0, O = 0;
  if (!clingo_symbol_number(Args[0], &N) ||
      !clingo_symbol_number(Args[1], &O))
    return false;

  NodeId = static_cast<unsigned>(N);
  OptIdx = static_cast<unsigned>(O);
  return true;
}

/// Pull all `select(N, O)` atoms out of a Clingo model.
bool extractSelectionsFromModel(
    clingo_model_t const *Model,
    std::vector<std::pair<unsigned, unsigned>> &Out) {
  size_t NumAtoms = 0;
  if (!clingo_model_symbols_size(Model, clingo_show_type_shown, &NumAtoms))
    return false;

  std::vector<clingo_symbol_t> Atoms(NumAtoms);
  if (!clingo_model_symbols(Model, clingo_show_type_shown, Atoms.data(),
                            NumAtoms))
    return false;

  Out.clear();
  Out.reserve(NumAtoms);
  for (auto Sym : Atoms) {
    unsigned NodeId = 0, OptIdx = 0;
    if (parseSelectAtom(Sym, NodeId, OptIdx))
      Out.emplace_back(NodeId, OptIdx);
  }
  return true;
}

#endif // LLVM_PBQP_HAVE_CLINGO

} // end anonymous namespace

namespace llvm {
namespace PBQP {
namespace RegAlloc {

#ifdef LLVM_PBQP_HAVE_CLINGO

Solution aspSolve(const PBQPRAGraph &G) {
  Solution Sol;

  std::string Program = encodeGraph(G);
  LLVM_DEBUG(dbgs() << "PBQP-ASP program:\n" << Program << "\n");

  // Create a Clingo control object configured for optimal-model search.
  // "--opt-mode=opt" is the default but stating it explicitly documents
  // intent and guards against future Clingo defaults changing.
  clingo_control_t *Ctl = nullptr;
  char const *ClingoArgs[] = {"--opt-mode=opt"};
  if (!clingo_control_new(ClingoArgs, /*nargs=*/1, /*logger=*/nullptr,
                          /*logger_data=*/nullptr,
                          /*message_limit=*/20, &Ctl)) {
    report_fatal_error("PBQP-ASP: failed to create Clingo control object");
  }

  if (!clingo_control_add(Ctl, "base", nullptr, 0, Program.c_str())) {
    clingo_control_free(Ctl);
    report_fatal_error("PBQP-ASP: failed to add ASP program to Clingo");
  }

  clingo_part_t Parts[] = {{"base", nullptr, 0}};
  if (!clingo_control_ground(Ctl, Parts, 1, nullptr, nullptr)) {
    clingo_control_free(Ctl);
    report_fatal_error("PBQP-ASP: failed to ground ASP program");
  }

  // Iterate models in yield mode. Under #minimize Clingo emits a sequence of
  // strictly-improving models; the last one before completion is the
  // proven-optimal allocation.
  clingo_solve_handle_t *Handle = nullptr;
  if (!clingo_control_solve(Ctl, clingo_solve_mode_yield, nullptr, 0,
                            /*notify=*/nullptr, /*data=*/nullptr, &Handle)) {
    clingo_control_free(Ctl);
    report_fatal_error("PBQP-ASP: failed to start solve");
  }

  std::vector<std::pair<unsigned, unsigned>> LatestSelections;
  bool FoundAny = false;

  while (true) {
    if (!clingo_solve_handle_resume(Handle))
      break;

    clingo_model_t const *Model = nullptr;
    if (!clingo_solve_handle_model(Handle, &Model))
      break;
    if (!Model)
      break; // No more models.

    std::vector<std::pair<unsigned, unsigned>> Current;
    if (extractSelectionsFromModel(Model, Current)) {
      LatestSelections = std::move(Current);
      FoundAny = true;
    }
  }

  clingo_solve_result_bitset_t Result = 0;
  clingo_solve_handle_get(Handle, &Result);
  clingo_solve_handle_close(Handle);
  clingo_control_free(Ctl);

  if (!FoundAny || !(Result & clingo_solve_result_satisfiable)) {
    report_fatal_error("PBQP-ASP: encoding is UNSAT (no valid allocation)");
  }

  LLVM_DEBUG({
    dbgs() << "PBQP-ASP optimal selections (" << LatestSelections.size()
           << "):\n";
    for (auto [NId, Opt] : LatestSelections)
      dbgs() << "  node " << NId << " -> option " << Opt << "\n";
  });

  for (auto [NId, Opt] : LatestSelections)
    Sol.setSelection(static_cast<GraphBase::NodeId>(NId), Opt);

  return Sol;
}

#else  // !LLVM_PBQP_HAVE_CLINGO

Solution aspSolve(const PBQPRAGraph &G) {
  report_fatal_error(
      "PBQP-ASP: LLVM was built without Clingo support. Reconfigure with "
      "-DLLVM_ENABLE_PROJECTS=\"clingo;...\" to enable the ASP solver.");
}

#endif // LLVM_PBQP_HAVE_CLINGO

} // namespace RegAlloc
} // namespace PBQP
} // namespace llvm
