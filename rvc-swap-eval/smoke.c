// Textbook candidates: a pointer live across a call with several compressible
// loads/stores afterwards. Each should produce one split.
extern int g(int);
extern void h(void);

int f1(int *p, int n) {          // p live across g(), 4 compressible loads after
  int acc = 0;
  for (int i = 0; i < n; i++) {
    acc += g(i);
    acc += p[0]; acc += p[1]; acc += p[2]; acc += p[3];
  }
  return acc;
}

int f2(int *p, int *q, int n) {  // two parents competing
  int acc = 0;
  h();
  acc += p[0] + p[1] + p[2];
  acc += q[0] + q[1] + q[2];
  h();
  acc += p[4] + q[4];
  return acc;
}

int f3(int *p, int c) {          // uses split across two successors (dominance)
  h();
  if (c) return p[0] + p[1] + p[2];
  return p[4] + p[5] + p[6];
}

int f4(int *p) {                 // single use: must be rejected by cost model
  h();
  return p[0];
}

long f5(long *p, int n) {        // RV64 ld/sd path
  long acc = 0;
  for (int i = 0; i < n; i++) { acc += g(i); acc += p[0] + p[1] + p[2] + p[3]; }
  return acc;
}
