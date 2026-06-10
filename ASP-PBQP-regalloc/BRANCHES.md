# Branch map — compression-aware ASP register allocation

Each branch explores a distinct mechanism for leveraging RVC compressibility.
Code-size deltas are `.text` bytes vs. plain greedy on the `-Os`
`rv64g_zca_zcb_zcmp` (zcmp) SPEC CPU2006 C/C++ subset (negative = smaller).

| Branch | Purpose | Status / result |
|--------|---------|-----------------|
| `main` | Integration trunk. Phase-1 GPRC hint/binding pass (`-riscv-asp-rvc-regalloc`) + single-phase asp-cs allocator (`-regalloc=riscv-asp-cs`). | baseline |
| `pressure-gated-asp` | Phase-1 pass with the GPRC pressure gate lowered 26→22. Gates register-bound functions back to greedy so binding only acts where it helps. | **−388 B net** (best code-size config). sjeng/h264ref flip from regressions to wins. |
| `asp-eviction-displacement` | Single-phase asp-cs extended so the solver may *displace* (spill) a vreg at a charged spill cost — a genuine assign-or-spill objective — plus a peak-pressure gate. | **Negative result.** Whole-range binding/spilling has no live-range splitting, so it loses to greedy; the regression is *not* pressure-correlated (bzip2 +98 at gates 12/16/22 alike) and cannot be gated away. Displacement wins only where splitting is irrelevant (mcf +4→−32). Documented in commit. |
| `asp-splitting` | Prototype a per-program-point ASP model that owns splitting + spilling itself (the only way a single-phase allocator can compete on high-pressure functions), then make it tractable via **region decomposition**. Standalone clingo, synthetic instances — *before* any LLVM integration. See `splitting/RESULTS.md` (monolithic) and `splitting/DECOMP_RESULTS.md` (decomposed). | **Monolithic: correct but doesn't scale** (proven optimum only ≤~50 instr; larger stalls). **Decomposed: tractability + quality restored.** Solving small windows (sweet spot W≈12) left-to-right with frozen entry boundaries beats monolithic ASP by 200–4000 objective points and 3–40× faster, scales linearly (i400 in 5.6 s). Next: LLVM integration to compare decomposed splitting vs greedy on real code. |

## Why splitting is the crux
Register allocation has two regimes. **Low pressure**: everything fits; the
only lever is compression (get compressible values into GPRC x8–x15) — a pure
assignment problem ASP solves optimally. **High pressure**: spilling is
unavoidable and spill code dominates code size. There, greedy *splits* a live
range and spills only the cold slice; the whole-range ASP model can only spill
entire ranges, which is strictly coarser and loses. Modelling splitting means
the decision variable becomes *location of value V at program point P*
(register r, or memory), with each location change costing a store/reload.
That is what `asp-splitting` prototypes.
