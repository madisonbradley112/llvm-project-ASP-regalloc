# Register Allocation Test Cases

## hard_regalloc_x86.c — ~24 NotProvablyAllocatable nodes (x86-64)

18 simultaneously-live `int64_t` accumulators on x86-64. With ~14 usable GP
registers, each accumulator has 17+ interference neighbours, exceeding NumOpts
and making all of them NotProvablyAllocatable. Additional live variables (loop
counter, pointer, `n`) push the actual hard sub-problem to ~24 nodes.

### Compile to LLVM IR

```bash
build/bin/clang -emit-llvm -S -O1 -target x86_64-linux-gnu \
    tests/hard_regalloc_x86.c -o /tmp/hard_x86.ll
```

### Allocate with hybrid ASP solver

```bash
build/bin/llc /tmp/hard_x86.ll -o /tmp/hard_x86.s \
    -mtriple=x86_64-linux-gnu \
    -regalloc=pbqp \
    -pbqp-use-asp-solver \
    -debug-only=pbqp-asp
```

Clingo statistics (models found, conflicts, CPU time) are printed after the
solve. Requires a debug build (`CMAKE_BUILD_TYPE=Debug`).

The solver uses a warm-start: a greedy pre-allocation is computed first and
passed to Clingo as `#heuristic` hints. Clingo finds the greedy solution (or
better) as its first model almost instantly, then improves it until the 30-second
time limit or optimality is proven. If the time limit expires before any model
is found, the greedy solution is used as a fallback. The debug output reports
which outcome occurred: `proven-optimal`, `sub-optimal (time limit hit)`, or
`no model before time limit — using greedy fallback`.

### Allocate with standard PBQP heuristic (reference)

```bash
build/bin/llc /tmp/hard_x86.ll -o /tmp/hard_x86_ref.s \
    -mtriple=x86_64-linux-gnu \
    -regalloc=pbqp
```

---

## hard_regalloc.c — ~56 NotProvablyAllocatable nodes (AArch64)

40 simultaneously-live `int64_t` accumulators on AArch64. With ~28 usable GP
registers, all 40 become NotProvablyAllocatable, and other live variables raise
the hard sub-problem to ~56 nodes. This size is not tractable for the ASP
solver; use it to verify that `partialReduce()` correctly classifies nodes and
to measure sub-problem isolation, not for end-to-end ASP solve tests.

### Compile to LLVM IR

```bash
build/bin/clang -emit-llvm -S -O1 -target aarch64-linux-gnu \
    tests/hard_regalloc.c -o /tmp/hard_aa64.ll
```

### Count hard nodes (without running ASP)

```bash
build/bin/llc /tmp/hard_aa64.ll -o /dev/null \
    -mtriple=aarch64-linux-gnu \
    -regalloc=pbqp \
    -pbqp-use-asp-solver \
    -debug-only=pbqp-asp 2>&1 | grep "PBQP-ASP: "
```

---

## Notes

- `-debug-only=pbqp-asp` requires a **debug build** (`CMAKE_BUILD_TYPE=Debug`
  with `-DLLVM_ENABLE_ASSERTIONS=ON`). Release builds silently ignore it.
- The hybrid solver path is enabled by `-pbqp-use-asp-solver` (flag defined in
  `llvm/lib/CodeGen/RegAllocPBQP.cpp`).
- Clingo statistics appear on stderr after the solve and include: models found,
  optimal model cost, conflicts, choices, and wall/CPU time.
- All commands assume the working directory is the repo root
  (`llvm-project-ASP-regalloc/`).
