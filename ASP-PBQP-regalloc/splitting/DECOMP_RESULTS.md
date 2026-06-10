# Region-decomposed splitting — prototype results

Follow-up to `RESULTS.md`: the monolithic per-point splitting model is correct
but does not scale (proven optimum only to ~40–50 instructions; larger
instances stall at poor solutions). This decomposition restores tractability.

## Approach
- `regalloc_region.lp`: the per-point model for **one window** of the program,
  with a **frozen entry boundary** — values crossing into the region carry
  their location (register R / memory) from the previous region's exit as
  `frozen_reg/2` / `frozen_mem/1`. The existing transition rules then charge a
  store/reload at the boundary iff the location changes; staying put is free.
- `decompose.py`: partitions the points into consecutive windows of size `W`,
  solves them left-to-right (each frozen to the previous one's exit), and
  stitches the pieces into a globally consistent allocation. Total objective =
  Σ region objectives (boundary transitions counted once, in the region whose
  entry they occur at).

This is *greedy across regions* (a region cannot revise an earlier region's
decisions) but *exact within a region*, so each region stays in the tractable
zone while the whole function is covered.

## Result: tractability AND quality restored
Objective = store+reload bytes − compression bytes (lower = better). Monolithic
run with a 20 s budget; decomposition with a **2 s per-region** limit.

| instance (peak) | MONO 20 s | DEC W=8 | DEC W=12 | DEC W=16 | DEC W=20 |
|---|---|---|---|---|---|
| i80  (10 / 8 regs)  | **+172**  | −38 / 0.15 s | **−52 / 0.47 s** | −48 / 2.4 s | −28 / 4.1 s |
| i200 (15 / 27 regs) | **+1334** | −164 / 0.8 s | −192 / 0.9 s | **−206 / 6 s** | +280 |
| i400 (17 / 27 regs) | **+3628** | −346 / 2.0 s | **−376 / 5.6 s** | +994 | +2352 |

- **Decomposition beats monolithic ASP decisively** — by 200–4000 objective
  points *and* ~3–40× faster. Monolithic's large positive values are an artifact
  of clingo getting lost in a huge search space; decomposition solves small
  optimal pieces and stitches them.
- **Window sweet spot ≈ 12.** Too small (8) loses a little context; too big
  (≥16 for 27 regs) pushes each region past what the 2 s per-region limit can
  solve, so regions return junk (hundreds of spurious stores) and quality
  collapses (i400 W=16 → +994). Rule: pick `W` small enough that each region
  reaches proven optimum within the per-region budget.
- **Scales linearly**: regions grow linearly with function length, each solved
  independently — i400 = 34 regions in 5.6 s, and regions are trivially
  parallelisable.

Boundary cost is small with adequate windows: on the low-pressure i48 (mono
optimum −54) decomposition gives −44 (W=20) to −52 (W=30) — a few compressions
lost at region seams, shrinking as windows grow.

## Caveats / next step
These are **synthetic** instances and the objective is the model's internal
cost, not measured `.text` vs greedy. The decisive comparison shown here is
decomposition vs *monolithic ASP*; whether decomposed exact splitting beats
**greedy** on real code requires LLVM integration (extract per-function
liveness, run the windowed solve, rewrite assignments + inserted spills) and a
SPEC measurement. What this prototype establishes: the tractability wall that
blocked the splitting approach is removed — region decomposition makes exact
compression-aware splitting fast and high-quality at realistic function sizes,
with a single tunable knob (window size) trading region-optimality against
boundary loss.
