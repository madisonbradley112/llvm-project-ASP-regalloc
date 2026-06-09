# Single-phase combinatorial register allocation — validation instances

Model: `../regalloc_singlephase_codesize.lp` (joint assignment + spilling +
RVC compression, byte-accurate code-size objective).

clingo reports `Optimization` = `spill_bytes + callee_saved_bytes − compression_savings`
(lower is better; negative means smaller than the uncompressed baseline).

| Instance | What it shows | Result |
|---|---|---|
| `decline_tradeoff.lp` | candidate cheap to spill, non-candidates expensive; one spill forced by pressure | optimum **2** — model *declines* compression, spills the cheap candidate |
| `decline_tradeoff.lp` + `force_all_compress.lp` | same, but every candidate forced to compress (binding-style) | **8** — forcing the compression costs an expensive spill (6 bytes worse) |
| `beneficial_spill.lp` | candidate expensive to spill, a near-free value available | optimum **−1** — model spills the cheap value to *keep* the compression |

Run:
```sh
clingo ../regalloc_singlephase_codesize.lp decline_tradeoff.lp --opt-mode=opt
clingo ../regalloc_singlephase_codesize.lp decline_tradeoff.lp force_all_compress.lp --opt-mode=opt
clingo ../regalloc_singlephase_codesize.lp beneficial_spill.lp --opt-mode=opt
```

These demonstrate the capability the phase-1+greedy (decoupled) design structurally
lacks: because spilling and compression are decided in one objective, the model
takes a compression only when its bytes saved exceed the spill bytes it induces.
