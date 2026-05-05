//===- RegAllocPBQPASP.cpp ----- ASP-based PBQP solver --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/RegAllocPBQPASP.h"
#include "llvm/CodeGen/RegAllocPBQP.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#ifdef LLVM_PBQP_HAVE_CLINGO
#include <clingo.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
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

// Clingo time limit in seconds.  Hard sub-problems that exceed this fall back
// to the greedy pre-allocation computed before the ASP call.
constexpr int ClingoTimeLimitSecs = 30;

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

/// Encode only a specified subset of nodes (and the edges between them) as
/// ASP facts.  After partialReduce(), hard nodes are only adjacent to other
/// hard nodes, so iterating their adjacency lists is sufficient to capture
/// all remaining edges without any additional filtering.
std::string encodeSubgraph(PBQPRAGraph &G,
                           const RegAllocSolverImpl::NodeSet &HardNodes) {
  std::ostringstream OS;
  OS << kPBQPProgramPrelude << "\n";

  for (auto NId : HardNodes) {
    OS << "node(" << NId << ").\n";
    const auto &NodeCosts = G.getNodeCosts(NId);
    for (unsigned I = 0, E = NodeCosts.getLength(); I != E; ++I) {
      OS << "option(" << NId << "," << I << ").\n";
      PBQPNum C = NodeCosts[I];
      if (isInfiniteCost(C)) {
        OS << "forbidden_node(" << NId << "," << I << ").\n";
      } else {
        OS << "node_cost(" << NId << "," << I << "," << scaleCost(C) << ").\n";
      }
    }
  }

  // Use a set of edge IDs to avoid emitting each edge twice (both endpoints
  // iterate the same edge).
  std::set<PBQPRAGraph::EdgeId> EmittedEdges;
  for (auto NId : HardNodes) {
    for (auto EId : G.adjEdgeIds(NId)) {
      if (!EmittedEdges.insert(EId).second)
        continue;
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
            OS << "edge_cost(" << N1Id << "," << N2Id << "," << I << "," << J
               << "," << scaleCost(C) << ").\n";
          }
        }
      }
    }
  }

  return OS.str();
}

/// Compute a greedy register assignment for the hard nodes.
///
/// Nodes are processed in descending spill-cost order (most expensive to spill
/// first), so variables that are costly to rematerialise get first pick of
/// physical registers.  For each node the cheapest finite option that does not
/// conflict with already-assigned neighbours is chosen.  Option 0 (spill) is
/// always a valid fallback because forbidden_edge never involves option 0 in
/// standard reg-alloc PBQP graphs.
///
/// The result is used both as a warm-start hint passed to Clingo and as a
/// fallback allocation when the time limit is hit before Clingo finds any model.
std::map<GraphBase::NodeId, unsigned>
computeGreedyHints(PBQPRAGraph &G,
                   const RegAllocSolverImpl::NodeSet &HardNodes) {
  std::vector<GraphBase::NodeId> Ordered(HardNodes.begin(), HardNodes.end());
  std::sort(Ordered.begin(), Ordered.end(),
            [&G](GraphBase::NodeId A, GraphBase::NodeId B) {
              return G.getNodeCosts(A)[0] > G.getNodeCosts(B)[0];
            });

  std::map<GraphBase::NodeId, unsigned> Assignment;

  for (auto NId : Ordered) {
    const auto &NodeCosts = G.getNodeCosts(NId);
    unsigned NumOpts = NodeCosts.getLength();

    // Collect options denied by already-assigned neighbours.
    std::set<unsigned> Denied;
    for (auto EId : G.adjEdgeIds(NId)) {
      auto N1Id = G.getEdgeNode1Id(EId);
      auto N2Id = G.getEdgeNode2Id(EId);
      auto OtherNId = (NId == N1Id) ? N2Id : N1Id;
      auto It = Assignment.find(OtherNId);
      if (It == Assignment.end())
        continue;
      unsigned OtherOpt = It->second;
      const auto &EC = G.getEdgeCosts(EId);
      for (unsigned I = 0; I < NumOpts; ++I) {
        PBQPNum C = (NId == N1Id) ? EC[I][OtherOpt] : EC[OtherOpt][I];
        if (isInfiniteCost(C))
          Denied.insert(I);
      }
    }

    // Pick the cheapest finite, non-denied option.  Defaults to option 0
    // (spill) which is always available.
    unsigned BestOpt = 0;
    PBQPNum BestCost = std::numeric_limits<PBQPNum>::infinity();
    for (unsigned I = 0; I < NumOpts; ++I) {
      if (Denied.count(I))
        continue;
      PBQPNum C = NodeCosts[I];
      if (isInfiniteCost(C))
        continue;
      if (C < BestCost) {
        BestCost = C;
        BestOpt = I;
      }
    }
    Assignment[NId] = BestOpt;
  }

  return Assignment;
}

/// Append warm-start hint facts and a #heuristic directive to an ASP program.
///
/// Each hint(N, O) fact tells Clingo's domain heuristic to prefer select(N, O)
/// when branching, guiding the search toward the greedy solution (or better)
/// as its first model.  Requires --heuristic=Domain in the solver arguments.
std::string
encodeHints(const std::map<GraphBase::NodeId, unsigned> &Hints) {
  std::ostringstream OS;
  OS << "\n% Warm-start: guide search toward greedy pre-allocation.\n";
  for (auto [NId, Opt] : Hints)
    OS << "hint(" << NId << "," << Opt << ").\n";
  OS << "#heuristic select(N,O) : hint(N,O). [1@2, true]\n";
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

enum class SolveStatus {
  Optimal,    ///< Proven optimal solution found.
  SubOptimal, ///< Best model found before time limit; may not be optimal.
  TimedOut,   ///< Time limit hit before any model was found; use greedy fallback.
  Unsat,      ///< No valid allocation exists (malformed graph).
};

/// Invoke Clingo on `Program` and return the best select/2 assignments found.
/// Uses a warm-start heuristic (--heuristic=Domain with hint/2 facts) so the
/// greedy pre-allocation is found as the first model almost instantly; Clingo
/// then improves it until the time limit or optimality is proven.
static SolveStatus runClingo(const std::string &Program,
                             std::vector<std::pair<unsigned, unsigned>> &Out) {
  clingo_control_t *Ctl = nullptr;
  // --heuristic=domain enables #heuristic directives (warm-start hints).
  // Clingo 5.8 has no --time-limit option; the wall-clock limit is enforced
  // by a timeout thread calling clingo_solve_handle_cancel below.
  char const *ClingoArgs[] = {"--opt-mode=opt", "--stats",
                               "--heuristic=domain"};
  if (!clingo_control_new(ClingoArgs, 3, nullptr, nullptr, 20, &Ctl)) {
    const char *Msg = clingo_error_message();
    std::string Err = "PBQP-ASP: failed to create Clingo control object";
    if (Msg)
      Err += std::string(": ") + Msg;
    report_fatal_error(Err.c_str());
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

  clingo_solve_handle_t *Handle = nullptr;
  if (!clingo_control_solve(Ctl, clingo_solve_mode_yield, nullptr, 0,
                            nullptr, nullptr, &Handle)) {
    clingo_control_free(Ctl);
    report_fatal_error("PBQP-ASP: failed to start solve");
  }

  // Timeout thread: interrupt the solve after ClingoTimeLimitSecs wall-clock
  // seconds.  clingo_control_interrupt is the thread-safe SIGINT equivalent;
  // it signals Clasp to stop at the next safe checkpoint, avoiding the race
  // condition that clingo_solve_handle_cancel has with decision-level cleanup.
  std::atomic<bool> SolveDone{false};
  std::thread TimeoutThread([&]() {
    auto Deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(ClingoTimeLimitSecs);
    while (!SolveDone.load(std::memory_order_relaxed)) {
      if (std::chrono::steady_clock::now() >= Deadline) {
        clingo_control_interrupt(Ctl);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });

  std::vector<std::pair<unsigned, unsigned>> Latest;
  bool FoundAny = false;

  while (true) {
    if (!clingo_solve_handle_resume(Handle))
      break;
    clingo_model_t const *Model = nullptr;
    if (!clingo_solve_handle_model(Handle, &Model))
      break;
    if (!Model)
      break;
    std::vector<std::pair<unsigned, unsigned>> Current;
    if (extractSelectionsFromModel(Model, Current)) {
      Latest = std::move(Current);
      FoundAny = true;
      LLVM_DEBUG(dbgs() << "PBQP-ASP: improved model found ("
                        << Latest.size() << " selections)\n");
    }
  }

  SolveDone.store(true, std::memory_order_relaxed);
  TimeoutThread.join();

  clingo_solve_result_bitset_t Result = 0;
  clingo_solve_handle_get(Handle, &Result);
  clingo_solve_handle_close(Handle);
  clingo_control_free(Ctl);

  bool Sat         = (Result & clingo_solve_result_satisfiable) != 0;
  bool Exhausted   = (Result & clingo_solve_result_exhausted)   != 0;
  bool Interrupted = (Result & clingo_solve_result_interrupted) != 0;

  if (!FoundAny) {
    if (Interrupted)
      return SolveStatus::TimedOut;
    if (!Sat)
      return SolveStatus::Unsat;
    return SolveStatus::TimedOut;
  }

  Out = std::move(Latest);
  return Exhausted ? SolveStatus::Optimal : SolveStatus::SubOptimal;
}

Solution aspSolve(PBQPRAGraph &G) {
  if (G.empty())
    return Solution();

  // Phase 1: polynomial preprocessing.
  // Apply R0/R1/R2 and conservative-allocation reduction rules.  After this,
  // only NotProvablyAllocatable nodes remain connected in the graph; all edges
  // to reduced nodes have already been one-side-disconnected, so the residual
  // graph is exactly the hard sub-problem.
  RegAllocSolverImpl Solver(G);
  G.setSolver(Solver);
  std::vector<GraphBase::NodeId> NodeStack = Solver.partialReduce();
  const RegAllocSolverImpl::NodeSet &HardNodes = Solver.getHardNodes();

  LLVM_DEBUG(dbgs() << "PBQP-ASP: " << NodeStack.size() << " easy node(s), "
                    << HardNodes.size() << " hard node(s)\n");

  // Sol will hold pre-seeded hard-node selections; easy nodes are filled in
  // by backpropagate() below.
  Solution Sol;

  if (!HardNodes.empty()) {
    // Phase 2: compute greedy warm-start, encode hard sub-problem, invoke Clingo.
    auto Hints = computeGreedyHints(G, HardNodes);
    std::string Program = encodeSubgraph(G, HardNodes) + encodeHints(Hints);
    LLVM_DEBUG(dbgs() << "PBQP-ASP hard-core program:\n" << Program << "\n");

    std::vector<std::pair<unsigned, unsigned>> Selections;
    SolveStatus Status = runClingo(Program, Selections);

    switch (Status) {
    case SolveStatus::Optimal:
      LLVM_DEBUG(dbgs() << "PBQP-ASP: proven-optimal solution ("
                        << Selections.size() << " hard nodes)\n");
      for (auto [NId, Opt] : Selections)
        Sol.setSelection(static_cast<GraphBase::NodeId>(NId), Opt);
      break;

    case SolveStatus::SubOptimal:
      LLVM_DEBUG(dbgs() << "PBQP-ASP: sub-optimal solution (time limit hit, "
                        << Selections.size() << " hard nodes)\n");
      for (auto [NId, Opt] : Selections)
        Sol.setSelection(static_cast<GraphBase::NodeId>(NId), Opt);
      break;

    case SolveStatus::TimedOut:
      LLVM_DEBUG(dbgs() << "PBQP-ASP: no model before time limit — "
                           "using greedy fallback\n");
      for (auto [NId, Opt] : Hints)
        Sol.setSelection(static_cast<GraphBase::NodeId>(NId), Opt);
      break;

    case SolveStatus::Unsat:
      report_fatal_error("PBQP-ASP: hard sub-problem is UNSAT");
    }
  }

  // Phase 3: backpropagate easy nodes.  Hard-node selections are pre-seeded
  // in Sol so that easy nodes adjacent to hard nodes can look them up.
  Solution Final = PBQP::backpropagate(G, std::move(NodeStack), std::move(Sol));
  G.unsetSolver();
  return Final;
}

#else  // !LLVM_PBQP_HAVE_CLINGO

Solution aspSolve(PBQPRAGraph &G) {
  report_fatal_error(
      "PBQP-ASP: LLVM was built without Clingo support. Reconfigure with "
      "-DLLVM_ENABLE_PROJECTS=\"clingo;...\" to enable the ASP solver.");
}

#endif // LLVM_PBQP_HAVE_CLINGO

} // namespace RegAlloc
} // namespace PBQP
} // namespace llvm
