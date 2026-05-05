/* hard_regalloc_x86.c — ~15 NotProvablyAllocatable nodes on x86-64
 *
 * Why x86-64, not AArch64
 * -----------------------
 * The minimum hard sub-problem size is determined by the number of
 * available physical registers (NumOpts) for a given register class.
 * A PBQP node is NotProvablyAllocatable only when its DeniedOpts ≥ NumOpts,
 * i.e. when it has at least NumOpts interference neighbours.  This means the
 * minimum hard sub-problem is a clique of exactly NumOpts+1 nodes.
 *
 * AArch64 exposes ~28 usable GP registers → minimum hard core = 29 nodes.
 * x86-64 exposes ~14 usable GP registers  → minimum hard core = 15 nodes.
 *
 * With 18 simultaneously-live int64 accumulators on x86-64 we expect a
 * hard sub-problem of roughly 15 nodes — small enough for the ASP solver
 * to finish in well under a second, while still being a genuinely hard
 * PBQP instance (no polynomial reduction rule can resolve it).
 *
 * Compile:
 *   clang -emit-llvm -S -O1 -target x86_64-linux-gnu \
 *         hard_regalloc_x86.c -o hard_regalloc_x86.ll
 * Allocate with hybrid ASP solver:
 *   llc hard_regalloc_x86.ll -regalloc=pbqp -pbqp-use-asp-solver \
 *       -debug-only=pbqp-asp -pbqp-export-results=hard_x86_asp.json
 * Allocate with standard PBQP (reference):
 *   llc hard_regalloc_x86.ll -regalloc=pbqp \
 *       -pbqp-export-results=hard_x86_ref.json
 */

#include <stdint.h>

extern void sink(int64_t);

int64_t hard_regalloc_x86(const int64_t *data, int64_t n,
                           int64_t hot_n, int64_t cold_n) {
    /* ---- hot accumulators (high spill cost) ---- */
    int64_t h00=0, h01=0, h02=0, h03=0, h04=0,
            h05=0, h06=0, h07=0, h08=0;

    /* ---- cold accumulators (low spill cost) ---- */
    int64_t c00=0, c01=0, c02=0, c03=0, c04=0,
            c05=0, c06=0, c07=0, c08=0;

    /* All 18 accumulators simultaneously live from here.
     * x86-64 PBQP sees ~14 usable GP registers; with 17+ interfering
     * neighbours each accumulator exceeds NumOpts → NotProvablyAllocatable. */

    for (int64_t i = 0; i < hot_n; i++) {
        int64_t v = data[i % n];
        h00+=v; h01+=v; h02+=v; h03+=v; h04+=v;
        h05+=v; h06+=v; h07+=v; h08+=v;
    }

    for (int64_t j = 0; j < cold_n; j++) {
        int64_t w = data[j % n];
        c00+=w; c01+=w; c02+=w; c03+=w; c04+=w;
        c05+=w; c06+=w; c07+=w; c08+=w;
    }

    int64_t result = h00+h01+h02+h03+h04+h05+h06+h07+h08+
                     c00+c01+c02+c03+c04+c05+c06+c07+c08;
    sink(result);
    return result;
}
