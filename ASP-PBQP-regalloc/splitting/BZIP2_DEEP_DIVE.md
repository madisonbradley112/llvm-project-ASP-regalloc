# Deep dive: the bzip2 `decompress.c` −1682 B "gain" (RV32E, asp-split)

Question investigated: why does `401.bzip2/decompress.c` shrink by 1682 bytes
(−18 %) under the asp-split pass on RV32E, while every other file in the suite
moves by at most ±~100 B — and is that gain real and leverageable?

**Answer: it was two stacked effects — a correctness bug (≈ −680 B, now fixed)
on top of a real, clean −1002 B (−10.9 %) that comes from the GPRC class
constraints alone, not from splitting.** The function's structure explains both
why the bug fired *here* and why the real gain is also largest *here*.

## 1. The fact pattern

Per-file deltas across the whole RV32E suite run (from the harness log):
`decompress.c` −1682 is a 2× outlier over #2 (`ComputeNonbondedUtil.C` −840,
namd) and #3 is `bzlib.c` −144 — **also bzip2, also containing resumable state
machines**. Everything else is within ±~100 B. Reproduction is deterministic
(base 3671 insns / 9202 B; buggy asp 3043 / 7520 B, bit-identical re-run).

## 2. What `BZ2_decompress` is, structurally

`decompress.c` is one function: a **resumable coroutine implemented in C**.
~30 scalar locals; **46 `GET_*` suspend/resume sites**; on entry all locals are
restored from `s->save_*` and on any suspend they are all saved back (72
`s->save_*` references). Consequently ~30 values are **live across nearly every
basic block**, with sparse clustered uses — and the function makes only **2
calls** in 600 lines. RV32E has ~13 allocatable GPRs (and only 2 callee-saved).

Baseline greedy *thrashes* on this: its own statistics for this one function
show **1302 copies inserted for splitting**, 656 reloads, 131 evictions, 764
re-queued ranges — the eviction/split cascade of 30 live-through values
fighting over 13 registers.

## 3. The bug (≈ −680 B of the gain; fixed)

The block-local model **pins all block-crossing values in registers** (the
"no cross-block splits" simplification). With ~30 live-through values and 13
registers, the one-value-per-register constraint is **pigeonhole-UNSAT**
(verified directly in clingo: 14 pinned values / 13 regs → UNSATISFIABLE; 13/13
→ SAT). Such windows return **no model at all** (6 committed windows in this
function; distinct from timeout-with-model). The transition scanner then read
the resulting gaps in the location map as "value in memory", fabricating
**reloads with no matching store**: 29 reloads vs 2 stores inserted, and the
binary contained **13 stack slots that are loaded but never written** (baseline:
0). The live ranges of those locals were chopped with garbage reloads → register
pressure artificially collapsed → greedy emitted far less spill/split code →
fake size win. `-verify-machineinstrs` cannot catch this (structurally valid
MIR; slot initialization is not checked).

**Fix (committed with this doc):** track per-point coverage by committed
models; a value whose in-block live range touches an unsolved (no-model) window
is never split (`NumSplitsSkippedGap`), and no-model windows are counted
(`NumWindowsNoModel`). After the fix the load-only-slot count is **0**.

## 4. The real gain (−1002 B, −10.9 %, clean)

With the fix, **zero splits are committed in this function** (all 38 candidate
splits cross gaps) — yet −1002 B remains. The only surviving intervention is
**1569 GPRC `constrainRegClass` constraints**, which are semantically safe by
construction (they only narrow the allocator's choices). Greedy's stats show
where the bytes come from — *less allocator churn*, plus better compression:

| greedy's own work (this function) | base | asp-fixed |
|---|---|---|
| copies inserted for splitting | 1302 | **909** |
| reloads inserted | 656 | **579** |
| interferences evicted | 131 | 104 |
| ranges re-queued | 764 | 612 |
| total instructions emitted | 3671 | **3379** |
| compressed-instruction share | 74.7 % | **78.7 %** |

Interpretation: on a function whose live-through values oversubscribe the
register file, greedy's local eviction heuristics churn (each split inserts
copies; each eviction re-queues). The ASP solution — computed globally per
window — acts as a **coordination signal**: constraining the compression
candidates to the GPRC subset (8 of 13 regs) effectively partitions the file
between short-lived computation values and long-lived state, damping the
eviction cascade. The win is mostly *fewer instructions* (−292), secondarily a
higher compressed share.

## 5. Why bzip2 and (almost) nothing else

The gain requires **all** of:
1. **live-through values ≫ register file** (~30 vs 13) — the regime where
   greedy thrashes and coordination has something to win. Other benchmarks'
   functions sit below this threshold (their windows are SAT, greedy is calm,
   deltas are ±noise).
2. **few calls** (2 in 600 lines). RV32E has only 2 callee-saved GPRs, so in
   call-dense code every value live across a call lives on the stack no matter
   what the allocator does — nothing to coordinate. lbm/namd on this march are
   *soft-float* (no F/D), i.e. maximally call-dense: +40/+176.
3. **a huge single function** — absolute bytes at stake scale with function
   size; greedy heuristic degradation grows with it.

Supporting evidence: the #3 gain (`bzlib.c` −144) contains the same
resumable-state-machine pattern (`handle_compress`, `BZ2_bzDecompress`), and
the #2 gain (`ComputeNonbondedUtil.C`, a giant generated kernel) matches
condition 1 and 3.

## 5b. Ablation: is the gain ASP-computed coordination, or generic class-narrowing?

Hypothesis tested: the constraints work because the solver selects a *jointly
consistent* subset of candidates (coordination), which a blanket heuristic
could not. Two arms, same files, same march/flags:
`-riscv-asp-split-naive-gprc` (no solver; constrain EVERY compression-candidate
vreg to GPRC) vs `-riscv-asp-split-constraints-only` (solver-selected
constraints, no splits).

| file | base | naive Δ (cons) | ASP-selected Δ (cons) |
|---|---|---|---|
| decompress.c | 9202 | **−1002** (838) | **−1002** (820) |
| bzlib.c      | 7192 | −140 (610) | −144 (607) |
| blocksort.c  | 5720 | +36 (297) | +34 (293) |
| sjeng.c      | 7578 | +82 (136) | +86 (111) |

**The hypothesis is refuted.** The no-solver arm reproduces the ASP arm to
within ±4 B on every file, including the identical −1002 on decompress. The
reason is visible in the constraint counts: the solver accepts nearly every
candidate anyway (820 of 838 on decompress), so "selected" ≈ "all" — the
selection carries almost no information. The churn-damping effect is real but
it is **generic class-narrowing**, achievable by a ~50-line heuristic pass with
no solver. Note also that neither arm discriminates the regime: both regress
equally on blocksort/sjeng, so the solver does not even predict *where*
narrowing helps; a separate regime gate (e.g. live-through count vs register
file — the same quantity whose excess makes the windows pigeonhole-UNSAT) would
be needed either way, and that quantity is directly computable without a solver.

## 6. Consequences

* **All asp-split suite numbers measured before the gap-guard fix are suspect**
  wherever no-model windows occurred (high-liveness functions). The suite must
  be re-run with the fixed compiler. mcf re-checked with the fixed compiler:
  **−60 B (was −56), clean** — its load-only slots (asp 9 vs baseline 16) are
  ordinary incoming stack-argument loads (ilp32e passes args 7+ on the stack),
  present in the pure-greedy baseline too; the decompress.c diagnostic was
  meaningful precisely because that leaf-like function has 0 in baseline.
* The headline finding inverts: in the extreme-pressure regime the *splitting*
  machinery (as currently designed, block-local with pinned boundaries) is
  **infeasible exactly where it was supposed to help** — the model goes UNSAT
  on the functions with the most to gain. What *does* win there is the cheap,
  safe part: **globally-computed GPRC class constraints**. This mirrors the
  project's phase-1 lesson (class constraints + greedy beat hard mechanisms).
* To make *splitting itself* contribute in this regime, the model must allow
  block-crossing values to live in memory across the boundary (cross-block
  spill coordination) instead of pinning them — a real extension, since the
  current pinning is what guarantees block-local correctness.
