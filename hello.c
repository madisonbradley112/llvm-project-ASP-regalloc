#include <stdio.h>
#include <stdint.h>

__attribute__((noinline))
int compute(int a,int b,int c,int d,int e,int f,int g,int h,int i,int j,int k,int l,int m,int n,int o,int p) {
    int s = 0;
    s += a*b + c - d;
    s += e*f + g - h;
    s += i*j + k - l;
    s += m*n + o - p;
    for (int x = 0; x < 1000; ++x) {
        s += (a ^ b) + (c & d) + (e | f) + (g << 1) + (h >> 1);
        s ^= x;
    }
    return s;
}

__attribute__((noinline))
int heavy(void) {
    volatile int t0 = 1, t1 = 2, t2 = 3, t3 = 4, t4 = 5, t5 = 6, t6 = 7, t7 = 8;
    volatile int t8 = 9, t9 = 10, t10 = 11, t11 = 12, t12 = 13, t13 = 14, t14 = 15, t15 = 16;
    int acc = 0;
    for (int i = 0; i < 500; ++i) {
        acc += t0 * t1 - t2 + t3;
        acc += t4 * t5 - t6 + t7;
        acc += t8 * t9 - t10 + t11;
        acc += t12 * t13 - t14 + t15;
        acc ^= i;
    }
    return acc;
}

int main(void) {
    int a=1,b=2,c=3,d=4,e=5,f=6,g=7,h=8,i=9,j=10,k=11,l=12,m=13,n=14,o=15,p=16;
    int r1 = compute(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p);
    int r2 = heavy();
    printf("compute=%d heavy=%d\n", r1, r2);
    return 0;
}
