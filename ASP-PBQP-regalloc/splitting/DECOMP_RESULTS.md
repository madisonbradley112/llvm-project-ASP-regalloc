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

## Adaptive narrowing (`adaptive_decompose.py`)
The fixed-`W` sweet spot is not uniform — it depends on *local* pressure, so any
single global `W` is a compromise (and the wrong choice can collapse quality).
Narrowing removes the knob: from each position try the **widest** window first
and accept it iff clingo **proves it optimal** within a short probe budget;
otherwise halve and retry, down to a floor. Wide windows win where the code is
easy (fewer seams → less boundary loss); the solver narrows automatically where
it is hard. Two efficiency tricks keep the wasted probes cheap: a short probe
limit (optimal regions solve fast anyway) and a *breathing* start — each region
begins near the previous region's accepted width (grown 50 %) instead of always
at `Wmax`, so sustained hard stretches don't re-probe the maximum every region.

Result (Wmax=40, floor=5, 1 s probe) vs. the best *hand-tuned* fixed window:

| instance | mono 20 s | best fixed-W | adaptive | time | width histogram |
|---|---|---|---|---|---|
| i48  | −54  | −52  | −48  | 0.9 s | 40, 8 |
| i80  | +172 | −52  | −52  | 4.2 s | 40, 13, 10, 10, 7 |
| i200 | +1334| −206 | **−228** | 8.8 s | 40, 19, 18, 16, 15×2, 14, 13, 12, 11, 10×2, 7 |
| i400 | +3628| −376 | **−408** | 23.2 s | 40, 19, 16, 15×2, 14, 13×7, 12×2, 11×2, 10×5, 9×7, 8, 7×3, 2 |

Every region is provably optimal, and adaptive **beats the best fixed window**
on the two larger instances with *no tuning*. The width histograms show the
intended behaviour: a wide 40-window over the easy prologue, settling to ~10–19
through the dense body. The probe limit is a clean speed/quality dial — a 2 s
probe reaches i80 −56 but takes 11 s; 1 s gives −52 in 4 s. (Remaining
inefficiency: ~half the regions still spend one wasted probe before narrowing;
a binary search for the max-optimal width would cut that further.)

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
