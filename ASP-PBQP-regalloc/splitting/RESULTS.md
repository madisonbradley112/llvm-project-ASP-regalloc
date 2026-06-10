# Per-program-point splitting model — prototype results

Standalone clingo prototype of a register allocator that owns live-range
**splitting** + spilling itself (the gap that makes the whole-range single-phase
model lose to greedy on high-pressure functions). Goal: determine, before any
LLVM integration, whether the model (a) makes the split-vs-spill-vs-keep
tradeoff correctly and (b) is tractable at realistic function sizes.

## Model (`regalloc_splitting.lp`)
Decision variable is the **location of value V at point P**: a register, or
memory. Key rules:
- `{ inreg(V,P,R) : reg(R) } 1 :- live(V,P).` — at most one register per point.
- one value per register per point (registers time-share across points);
- uses/defs must be in a register (no memory operands);
- no free register-to-register moves (must pass through memory);
- `store`/`reload` = the reg↔mem transitions = the static spill instructions;
- `realized(I)` compression iff the value sits in a gprc register at the use.
- Objective: minimize store+reload bytes, maximize compression bytes.

## Correctness — PASS
- **t1_split_vs_spill**: 2 regs, three values, pressure 3 at the busy middle.
  The solver *splits* the long endpoint-only value (`store` after its first use,
  `reload` before its last) instead of spilling the actively-used values.
  Optimum = 8 (1 store + 1 reload), the unique minimum. ✔ This is greedy's
  splitting behaviour, derived optimally.
- **t2_split_plus_compress**: same, with the held value's uses compressible and
  reg 0 a gprc reg. Solver splits the long value AND keeps the compressible one
  in the gprc reg for its whole span → all 7 candidates realized. Optimum =
  8 − 14 = −6. ✔ Splitting and compression compose correctly.

## Tractability — FAILS to scale (monolithic whole-function solve)
Realistic straight-line instances (`gen.py`: one instruction per point, ≤2
operands per instruction, pressure from liveness). `clingo --opt-mode=opt`.

Proven-optimum reach (peak pressure ≥ regs, i.e. spilling required):

| instructions | regs | peak | result | solve |
|---|---|---|---|---|
| 40 | 8 | 8 | **OPTIMUM** | 0.19 s |
| 60 | 8 | 9 | anytime (no proof) | hit 20 s |
| 80 | 8 | 10 | anytime | hit 20 s |
| 120–300 | 27 | 15–17 | anytime | hit 20 s |

Anytime quality (best objective vs wall-clock budget; lower = better):

| budget | sc80 (80 instr) | sc200 (200 instr) |
|---|---|---|
| 1 s | 324 | (none found) |
| 2 s | 300 | 1342 |
| 5 s | 276 | 1334 |
| 20 s | 172 | 1334 (stuck) |

- Proven optimum only up to **~40–50 instructions** once spilling is forced.
- At 80 instructions a **2 s** budget is **~75 % worse** than the 20 s solution,
  and 20 s is still not optimal — the objective is still falling.
- At 200 instructions the solver finds a mediocre solution and **stalls**
  (no improvement 5 s → 20 s).

## Conclusion
The per-point splitting model is **correct** but the **monolithic whole-function
solve is not tractable** at realistic sizes: usable per-function time budgets
(~2 s) give poor anytime quality for functions beyond a few dozen instructions,
and SPEC functions routinely run to hundreds. Plugging this in as-is would not
beat greedy.

The result points at the necessary next step if this track is pursued:
**decomposition** — solve splitting exactly on small regions (basic blocks,
loops, or live-range bundles) rather than the whole function, and/or
**warm-start** clingo from greedy's allocation so it only refines. This mirrors
how production optimal allocators and greedy's own region-splitting stay
tractable. Without that, the gated phase-1 class-constraint approach
(`pressure-gated-asp`, −388 B) remains the practical code-size win.
