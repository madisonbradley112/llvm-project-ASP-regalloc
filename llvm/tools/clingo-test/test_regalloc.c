// test_regalloc.c — Register allocation stress test
//
// Designed to produce meaningful register pressure for PBQP:
//   - Many live variables simultaneously (forces interesting allocation choices)
//   - A loop with a reduction (live-across-loop values)
//   - An arithmetic-heavy function (many temps, no calls = no spill from ABI)
//   - A caller that exercises call-site register saving
//
// Build with:
//   clang -O1 -emit-llvm -S -o test_regalloc.ll test_regalloc.c
//
// Then allocate:
//   llc -regalloc=pbqp                   -mtriple=x86_64 test_regalloc.ll -o pbqp.s
//   llc -regalloc=pbqp -pbqp-use-asp-solver -mtriple=x86_64 test_regalloc.ll -o asp.s
//
// Export results for comparison:
//   llc -regalloc=pbqp                      -pbqp-export-results=pbqp.json -mtriple=x86_64 test_regalloc.ll -o /dev/null
//   llc -regalloc=pbqp -pbqp-use-asp-solver -pbqp-export-results=asp.json  -mtriple=x86_64 test_regalloc.ll -o /dev/null

#include <stdio.h>

// Many live values at once — exercises register pressure.
// At the point of the final sum, a0..a9 and b0..b9 are all live.
static int pressure(int x) {
    int a0 = x + 1,  a1 = x + 2,  a2 = x + 3,  a3 = x + 4,  a4 = x + 5;
    int a5 = x + 6,  a6 = x + 7,  a7 = x + 8,  a8 = x + 9,  a9 = x + 10;
    int b0 = x * 1,  b1 = x * 2,  b2 = x * 3,  b3 = x * 4,  b4 = x * 5;
    int b5 = x * 6,  b6 = x * 7,  b7 = x * 8,  b8 = x * 9,  b9 = x * 10;

    // Interleaved use keeps all 20 values live to this point.
    return (a0 + b9) - (a1 + b8) + (a2 + b7) - (a3 + b6)
         + (a4 + b5) - (a5 + b4) + (a6 + b3) - (a7 + b2)
         + (a8 + b1) - (a9 + b0);
}

// Loop with a live-across-iteration accumulator — tests live ranges that
// span back-edges, which affects PBQP edge costs.
static int dot_product(const int *a, const int *b, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        int ai = a[i];
        int bi = b[i];
        int prod = ai * bi;
        sum += prod;
    }
    return sum;
}

// Nested arithmetic with interfering temporaries.
static int arithmetic(int a, int b, int c, int d) {
    int t0 = a + b;
    int t1 = c + d;
    int t2 = a * c;
    int t3 = b * d;
    int t4 = t0 * t1;
    int t5 = t2 + t3;
    int t6 = t4 - t5;
    int t7 = t6 * t6;
    return t7 + t0 + t1 + t2 + t3;
}

// Exercises call-site register saving (caller-saved vs callee-saved choices).
static int caller(int x, int n) {
    int a[8] = {x, x+1, x+2, x+3, x+4, x+5, x+6, x+7};
    int b[8] = {x+8, x+7, x+6, x+5, x+4, x+3, x+2, x+1};
    int dp = dot_product(a, b, 8);
    int p  = pressure(x);
    int ar = arithmetic(x, x+1, x+2, x+3);
    return dp + p + ar;
}

int main(void) {
    int result = caller(3, 8);
    printf("result = %d\n", result);

    // Known-answer check so we notice miscompilation.
    // With x=3, n=8:
    //   dot_product: sum of (3+i)*(11-i) for i=0..7
    //     = 3*11 + 4*10 + 5*9 + 6*8 + 7*7 + 8*6 + 9*5 + 10*4
    //     = 33 + 40 + 45 + 48 + 49 + 48 + 45 + 40 = 348
    //   pressure(3): sum collapses to (a0-a9 interleaved with b0-b9)
    //     each pair (a_k + b_{9-k}) contributes (3+k+1) + (3*(10-k))
    //     = 4+k + 30-3k = 34-2k, then alternating signs
    //     = (34-0) - (34-2) + (34-4) - (34-6) + ... for k=0..9
    //     = 34 - 32 + 30 - 28 + 26 - 24 + 22 - 20 + 18 - 16 = 10
    //   arithmetic(3,4,5,6):
    //     t0=7 t1=11 t2=15 t3=24 t4=77 t5=39 t6=38 t7=1444
    //     return 1444+7+11+15+24 = 1501
    //   total = 348 + 10 + 1501 = 1859
    int expected = 1859;
    if (result != expected) {
        printf("FAIL: expected %d, got %d\n", expected, result);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
