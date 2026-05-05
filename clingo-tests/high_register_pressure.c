/* High register-pressure test for PBQP regalloc comparison
 * This creates real register allocation pressure by maintaining
 * many live values simultaneously with overlapping lifetimes.
 * The loop forces the allocator to make non-trivial spilling decisions.
 */

#include <stdint.h>

/* Volatile sink to prevent optimization */
extern void use_value(int32_t x);


/* High-pressure computation: 16 parallel accumulators in a loop
 * Each accumulator is live throughout, forcing significant register pressure.
 * With only ~28 usable registers on AArch64 and 16 accumulators,
 * the PBQP solver must make spilling/coalescing decisions.
 */
int32_t high_register_pressure(const int32_t *data, int32_t n) {
    int32_t sum0 = 0, sum1 = 0, sum2 = 0, sum3 = 0;
    int32_t sum4 = 0, sum5 = 0, sum6 = 0, sum7 = 0;
    int32_t sum8 = 0, sum9 = 0, sum10 = 0, sum11 = 0;
    int32_t sum12 = 0, sum13 = 0, sum14 = 0, sum15 = 0;

    /* Loop: keep all 16 accumulators live with overlapping operations */
    for (int32_t i = 0; i < n; i++) {
        int32_t val = data[i];
        
        /* Each accumulator is updated with a value-dependent operation,
         * creating interference and dependencies that force allocation decisions */
        sum0 += val * (i + 0);
        sum1 += val * (i + 1);
        sum2 += val * (i + 2);
        sum3 += val * (i + 3);
        sum4 += val * (i + 4);
        sum5 += val * (i + 5);
        sum6 += val * (i + 6);
        sum7 += val * (i + 7);
        sum8 += val * (i + 8);
        sum9 += val * (i + 9);
        sum10 += val * (i + 10);
        sum11 += val * (i + 11);
        sum12 += val * (i + 12);
        sum13 += val * (i + 13);
        sum14 += val * (i + 14);
        sum15 += val * (i + 15);
    }

    /* Combine results: forces all 16 values to be live simultaneously */
    int32_t result = sum0 + sum1 + sum2 + sum3 + sum4 + sum5 + sum6 + sum7 +
                     sum8 + sum9 + sum10 + sum11 + sum12 + sum13 + sum14 + sum15;

    /* Volatile use prevents aggressive optimization */
    use_value(result);
    
    return result;
}

/* Test driver to ensure high_register_pressure is not optimized away */
int32_t driver() {
    static const int32_t test_data[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
        11, 12, 13, 14, 15, 16, 17, 18, 19, 20
    };
    
    return high_register_pressure(test_data, 20);
}
