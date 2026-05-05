/* coalesce_pressure_x86.c — coalescing edges among NPA nodes on x86-64
 *
 * Motivation
 * ----------
 * The pure-interference test (hard_regalloc_x86.c) showed no improvement from
 * ASP because greedy-by-spill-cost is already optimal when there are no
 * coalescing edges.  This function adds coalescing structure: the cold
 * accumulators are initialised from the hot accumulators via phi-nodes, so the
 * PBQP graph contains negative-cost coalescing edges between them.  Assigning
 * a hot and its paired cold to the same register saves a copy; the greedy
 * heuristic may miss this when register pressure prevents the obvious choice.
 *
 * Structure
 * ---------
 * hot[0..8] accumulate in a hot loop (high spill cost).
 * cold[0..8] are conditionally copied from the corresponding hot value or
 * start at zero (branch creates phi-nodes → coalescing edges in machine IR).
 * Both groups are simultaneously live throughout the second loop, creating
 * ~18 NPA nodes with coalescing edges between paired hot/cold values.
 *
 * Compile:
 *   build/bin/clang -emit-llvm -S -O1 -target x86_64-linux-gnu \
 *       tests/coalesce_pressure_x86.c -o /tmp/coalesce_x86.ll
 * Allocate (ASP):
 *   build/bin/llc /tmp/coalesce_x86.ll -o /tmp/coalesce_x86_asp.s \
 *       -mtriple=x86_64-linux-gnu -regalloc=pbqp -pbqp-use-asp-solver \
 *       -debug-only=pbqp-asp
 * Allocate (reference):
 *   build/bin/llc /tmp/coalesce_x86.ll -o /tmp/coalesce_x86_ref.s \
 *       -mtriple=x86_64-linux-gnu -regalloc=pbqp
 */

#include <stdint.h>
extern void sink(int64_t);

int64_t coalesce_pressure_x86(const int64_t *data, int64_t n,
                               int64_t hot_n, int64_t use_hot) {
    int64_t h0=0, h1=0, h2=0, h3=0, h4=0, h5=0, h6=0, h7=0, h8=0;

    for (int64_t i = 0; i < hot_n; i++) {
        int64_t v = data[i % n];
        h0+=v; h1+=v; h2+=v; h3+=v; h4+=v;
        h5+=v; h6+=v; h7+=v; h8+=v;
    }

    /* Branch on use_hot: creates phi-nodes in the IR, which lower to copy
     * instructions in machine IR.  The PBQP allocator builds coalescing edges
     * between each h[i] and c[i] — exactly the structure we need. */
    int64_t c0, c1, c2, c3, c4, c5, c6, c7, c8;
    if (use_hot) {
        c0=h0; c1=h1; c2=h2; c3=h3; c4=h4;
        c5=h5; c6=h6; c7=h7; c8=h8;
    } else {
        c0=0; c1=0; c2=0; c3=0; c4=0;
        c5=0; c6=0; c7=0; c8=0;
    }

    /* Both groups live simultaneously in this loop. */
    int64_t result = 0;
    for (int64_t j = 0; j < n; j++) {
        int64_t v = data[j];
        result += h0+h1+h2+h3+h4+h5+h6+h7+h8;
        result += c0+c1+c2+c3+c4+c5+c6+c7+c8;
        result ^= v;
    }

    sink(result);
    return result;
}
