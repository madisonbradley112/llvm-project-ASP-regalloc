/* call_pressure_x86.c — ABI copy coalescing under register pressure (x86-64)
 *
 * Motivation
 * ----------
 * On x86-64, function arguments are passed in rdi/rsi/rdx/rcx/r8/r9.
 * When a live variable already resides in the right ABI register for an
 * upcoming call, PBQP can coalesce the VReg with that physical register at
 * zero copy cost.  Under high register pressure the greedy heuristic may
 * assign the variable to a different register and then emit a mov before the
 * call; ASP can see the full picture and choose register assignments that
 * avoid those moves globally.
 *
 * Structure
 * ---------
 * Four accumulators (a, b, c, d) build up values across a pressure loop.
 * They are then passed as arguments to two calls in a fixed order.  With 14
 * other simultaneously-live pressure variables the accumulators become NPA
 * nodes; the ABI copies create coalescing edges between each accumulator and
 * its target argument register.  Globally, at most one assignment can satisfy
 * all four coalescing edges simultaneously — the greedy heuristic may pick a
 * worse permutation.
 *
 * Compile:
 *   build/bin/clang -emit-llvm -S -O1 -target x86_64-linux-gnu \
 *       tests/call_pressure_x86.c -o /tmp/call_x86.ll
 * Allocate (ASP):
 *   build/bin/llc /tmp/call_x86.ll -o /tmp/call_x86_asp.s \
 *       -mtriple=x86_64-linux-gnu -regalloc=pbqp -pbqp-use-asp-solver \
 *       -debug-only=pbqp-asp
 * Allocate (reference):
 *   build/bin/llc /tmp/call_x86.ll -o /tmp/call_x86_ref.s \
 *       -mtriple=x86_64-linux-gnu -regalloc=pbqp
 */

#include <stdint.h>
extern void sink4(int64_t, int64_t, int64_t, int64_t);
extern void sink(int64_t);

int64_t call_pressure_x86(const int64_t *data, int64_t n) {
    /* Four primary accumulators — candidates for ABI register coalescing. */
    int64_t a=0, b=0, c=0, d=0;

    /* 14 pressure accumulators to push a,b,c,d into NPA territory. */
    int64_t p0=0,p1=0,p2=0,p3=0,p4=0,p5=0,p6=0,
            p7=0,p8=0,p9=0,p10=0,p11=0,p12=0,p13=0;

    for (int64_t i = 0; i < n; i++) {
        int64_t v = data[i];
        a+=v; b+=v*2; c+=v*3; d+=v*4;
        p0+=v;  p1+=v;  p2+=v;  p3+=v;  p4+=v;  p5+=v;  p6+=v;
        p7+=v;  p8+=v;  p9+=v;  p10+=v; p11+=v; p12+=v; p13+=v;
    }

    /* Call 1: pass a,b,c,d as first four arguments.
     * Coalescing: a→rdi, b→rsi, c→rdx, d→rcx would cost 0 copies. */
    sink4(a, b, c, d);

    /* Call 2: pass in reverse order — only one permutation can satisfy both
     * calls without copies; ASP can find it, greedy may not. */
    sink4(d, c, b, a);

    int64_t result = a+b+c+d+p0+p1+p2+p3+p4+p5+p6+p7+p8+p9+p10+p11+p12+p13;
    sink(result);
    return result;
}
