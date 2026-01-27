#include "clingo.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>
#include <chrono>
#include <cstring>
#include <sstream>

// Benchmark the N-Queens problem
bool benchmark_nqueens(unsigned int n) {
  llvm::outs() << "\n  Benchmarking " << n << "-Queens...\n";
  
  clingo_control_t *ctl = nullptr;
  
  // Create control object
  if (!clingo_control_new(nullptr, 0, nullptr, nullptr, 1, &ctl)) {
    llvm::errs() << "    ✗ Failed to create control object\n";
    return false;
  }
  
  // N-Queens ASP program
  std::ostringstream program_stream;
  program_stream 
    << "% N-Queens problem\n"
    << "n(" << n << ").\n"
    << "pos(1.." << n << ").\n"
    << "\n"
    << "% Generate candidate solution\n"
    << "{queen(R,C) : pos(C)} = 1 :- pos(R).\n"
    << "\n"
    << "% Constraint: no two queens in same column\n"
    << ":- queen(R1,C), queen(R2,C), R1 < R2.\n"
    << "\n"
    << "% Constraint: no two queens in same diagonal\n"
    << ":- queen(R1,C1), queen(R2,C2), R1 < R2, |C1-C2| = |R1-R2|.\n";
  
//  llvm::outs() << "  -------------------------\n";
  std::string program = program_stream.str();
  
//   llvm::outs() << "\n  Generated clingo program:\n\n";
//   llvm::outs() << program << "\n";  
//   llvm::outs() << "  -------------------------\n";

  // Add program
  if (!clingo_control_add(ctl, "base", nullptr, 0, program.c_str())) {
    llvm::errs() << "    ✗ Failed to add program\n";
    clingo_control_free(ctl);
    return false;
  }
  
  // Ground
  auto ground_start = std::chrono::high_resolution_clock::now();
  
  clingo_part_t parts[] = {{"base", nullptr, 0}};
  if (!clingo_control_ground(ctl, parts, 1, nullptr, nullptr)) {
    llvm::errs() << "    ✗ Failed to ground program\n";
    clingo_control_free(ctl);
    return false;
  }
  
  auto ground_end = std::chrono::high_resolution_clock::now();
  auto ground_time = std::chrono::duration_cast<std::chrono::milliseconds>(
    ground_end - ground_start).count();
  
  // Solve with model counting
  auto solve_start = std::chrono::high_resolution_clock::now();
  
  clingo_solve_handle_t *handle = nullptr;
  if (!clingo_control_solve(ctl, clingo_solve_mode_yield, nullptr, 0, nullptr, nullptr, &handle)) {
    llvm::errs() << "    ✗ Failed to create solve handle\n";
    clingo_control_free(ctl);
    return false;
  }
  
  // Get the final result (this exhausts all models)
  clingo_solve_result_bitset_t result;
  if (!clingo_solve_handle_get(handle, &result)) {
    llvm::errs() << "    ✗ Failed to get solve result\n";
    clingo_solve_handle_close(handle);
    clingo_control_free(ctl);
    return false;
  }
  
  clingo_solve_handle_close(handle);
  
  auto solve_end = std::chrono::high_resolution_clock::now();
  auto solve_time = std::chrono::duration_cast<std::chrono::milliseconds>(
    solve_end - solve_start).count();
  
  // Determine if satisfiable
  bool satisfiable = (result & clingo_solve_result_satisfiable) != 0;
  llvm::outs() << "    ✓ Problem is " << (satisfiable ? "satisfiable" : "unsatisfiable") << "\n";
  llvm::outs() << "    ✓ Ground time: " << ground_time << " ms\n";
  llvm::outs() << "    ✓ Solve time:  " << solve_time << " ms\n";
  llvm::outs() << "    ✓ Total time:  " << (ground_time + solve_time) << " ms\n";
  
  // Cleanup
  clingo_control_free(ctl);
  return true;
}

int main(int argc, char *argv[]) {
  llvm::outs() << "╔════════════════════════════════════════════════════════════╗\n";
  llvm::outs() << "║         Clingo C API Integration Benchmark for LLVM        ║\n";
  llvm::outs() << "╚════════════════════════════════════════════════════════════╝\n";
  
  // Test 1: Simple test
  llvm::outs() << "\n1. Basic Clingo Functionality Test\n";
  llvm::outs() << "   ─────────────────────────────────\n";
  
  clingo_control_t *ctl = nullptr;
  
  if (!clingo_control_new(nullptr, 0, nullptr, nullptr, 1, &ctl)) {
    llvm::errs() << "   ✗ Failed to create control object\n";
    return 1;
  }
  llvm::outs() << "   ✓ Created clingo control object\n";
  
  const char *simple_program = "fact(1..3).";
  if (!clingo_control_add(ctl, "base", nullptr, 0, simple_program)) {
    llvm::errs() << "   ✗ Failed to add program\n";
    clingo_control_free(ctl);
    return 1;
  }
  llvm::outs() << "   ✓ Added logic program: \"fact(1..3).\"\n";
  
  clingo_part_t parts[] = {{"base", nullptr, 0}};
  if (!clingo_control_ground(ctl, parts, 1, nullptr, nullptr)) {
    llvm::errs() << "   ✗ Failed to ground program\n";
    clingo_control_free(ctl);
    return 1;
  }
  llvm::outs() << "   ✓ Successfully grounded program\n";
  
  clingo_control_free(ctl);
  
  // Test 2: N-Queens Benchmark
  llvm::outs() << "\n2. N-Queens Problem Benchmark\n";
  llvm::outs() << "   ─────────────────────────────\n";
  llvm::outs() << "   (Classic ASP constraint satisfaction problem)\n";
  
  // Small benchmarks
  if (!benchmark_nqueens(8)) {
    llvm::errs() << "\n   ✗ 8-Queens benchmark failed\n";
    return 1;
  }
  
  if (!benchmark_nqueens(12)) {
    llvm::errs() << "\n   ✗ 12-Queens benchmark failed\n";
    return 1;
  }
  
  if (!benchmark_nqueens(16)) {
    llvm::errs() << "\n   ✗ 16-Queens benchmark failed\n";
    return 1;
  }
  
  // Summary
  llvm::outs() << "\n╔════════════════════════════════════════════════════════════╗\n";
  llvm::outs() << "║                      BENCHMARK COMPLETE                    ║\n";
  llvm::outs() << "║                                                            ║\n";
  llvm::outs() << "║  ✓ Clingo C API integration with LLVM working perfectly!   ║\n";
  llvm::outs() << "╚════════════════════════════════════════════════════════════╝\n";
  
  return 0;
}
