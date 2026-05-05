/* hard_regalloc.c — NotProvablyAllocatable PBQP test case
 *
 * Purpose
 * -------
 * Demonstrate that the hybrid ASP solver correctly identifies and isolates
 * the NotProvablyAllocatable (hard) sub-problem from the rest of the
 * PBQP graph.  The hard sub-problem is the set of nodes where PBQP
 * reduction rules (R0/R1/R2) and conservative allocation cannot guarantee
 * a register — these are the NP-hard decisions left for the solver.
 *
 * Why clique-based pressure creates hard nodes
 * --------------------------------------------
 * A PBQP node is NotProvablyAllocatable when every register option it has
 * is "denied" by interference edges to enough neighbours.  On AArch64 the
 * PBQP allocator sees ≈28 usable GP registers.  If N variables are all
 * simultaneously live they form a clique of size N in the interference
 * graph.  Each node then has N-1 interfering neighbours; when N-1 ≥ 28
 * every register option is potentially blocked → all N nodes are
 * NotProvablyAllocatable.
 *
 * With 40 accumulators here (N=40, N-1=39 ≫ 28) all 40+ accumulator
 * VRegs land in the hard sub-problem.  The hybrid solver correctly
 * extracts this sub-problem (log line "X hard node(s)") and would hand
 * it to Clingo; however a 40-node clique coloring with 28 colors requires
 * choosing which 12 variables to spill — C(40,12) ≈ 5.6 × 10⁹ candidates
 * — which exceeds practical ASP solve time.
 *
 * Use this file to:
 *   (a) verify that partialReduce() correctly classifies nodes as hard,
 *   (b) measure the size of the hard sub-problem before and after the
 *       hybrid pre-processing step,
 *   (c) compare its spill decisions with the standard PBQP heuristic.
 *
 * For end-to-end ASP solve tests use a function whose hard sub-problem
 * has ≲15 nodes (e.g. the Scholz-Eckstein benchmark in ASP-PBQP-regalloc/).
 *
 * Compile:
 *   clang -emit-llvm -S -O1 -target aarch64-linux-gnu hard_regalloc.c \
 *         -o hard_regalloc.ll
 * Allocate:
 *   llc hard_regalloc.ll -regalloc=pbqp -pbqp-use-asp-solver \
 *       -debug-only=pbqp-asp -pbqp-export-results=hard_asp.json
 */

#include <stdint.h>

extern void sink(int64_t);

int64_t hard_regalloc(const int64_t *data, int64_t n,
                      int64_t hot_n, int64_t cold_n) {
    /* ---- hot accumulators (high spill cost: hot_n loop iterations) ---- */
    int64_t h00=0, h01=0, h02=0, h03=0, h04=0, h05=0, h06=0, h07=0,
            h08=0, h09=0, h10=0, h11=0, h12=0, h13=0, h14=0, h15=0,
            h16=0, h17=0, h18=0, h19=0;

    /* ---- cold accumulators (low spill cost: cold_n loop iterations) ---- */
    int64_t c00=0, c01=0, c02=0, c03=0, c04=0, c05=0, c06=0, c07=0,
            c08=0, c09=0, c10=0, c11=0, c12=0, c13=0, c14=0, c15=0,
            c16=0, c17=0, c18=0, c19=0;

    /* All 40 accumulators live from this point onward.  They interfere
     * pairwise (39 neighbours each, 39 ≫ 28 available GP regs) so all
     * become NotProvablyAllocatable nodes in the PBQP graph. */

    for (int64_t i = 0; i < hot_n; i++) {
        int64_t v = data[i % n];
        h00+=v; h01+=v; h02+=v; h03+=v; h04+=v; h05+=v; h06+=v; h07+=v;
        h08+=v; h09+=v; h10+=v; h11+=v; h12+=v; h13+=v; h14+=v; h15+=v;
        h16+=v; h17+=v; h18+=v; h19+=v;
    }

    for (int64_t j = 0; j < cold_n; j++) {
        int64_t w = data[j % n];
        c00+=w; c01+=w; c02+=w; c03+=w; c04+=w; c05+=w; c06+=w; c07+=w;
        c08+=w; c09+=w; c10+=w; c11+=w; c12+=w; c13+=w; c14+=w; c15+=w;
        c16+=w; c17+=w; c18+=w; c19+=w;
    }

    int64_t result =
        h00+h01+h02+h03+h04+h05+h06+h07+h08+h09+
        h10+h11+h12+h13+h14+h15+h16+h17+h18+h19+
        c00+c01+c02+c03+c04+c05+c06+c07+c08+c09+
        c10+c11+c12+c13+c14+c15+c16+c17+c18+c19;
    sink(result);
    return result;
}
