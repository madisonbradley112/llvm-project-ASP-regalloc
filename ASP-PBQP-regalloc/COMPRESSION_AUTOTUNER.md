# Compression-aware register narrowing + per-function autotuner

A code-size optimization for RISC-V: bias register allocation toward the
compressed-instruction register subset (`x8`–`x15`) so that more instructions
emit in their 2-byte RVC form, then use a small empirical autotuner to apply
that bias only where it actually shrinks each function.

The mechanism lives **in-tree** (an LLVM pass, `RISCVASPSplit.cpp`); the search
that decides *where* to apply it lives **out-of-tree** (shell harnesses in this
directory). See "How it fits together" for why the split is intentional.

---

## What the change does

RISC-V's "C" (compressed) extension encodes common instructions in 2 bytes
instead of 4. Several compressed formats (CL/CS/CIW/CA/CB — register-register
ALU ops and the non-stack loads/stores) can only name **8** of the 32 integer
registers: `x8`–`x15` (the `GPRC` register class). If an instruction's operands
happen to be allocated outside that window, it cannot compress and stays 4
bytes.

The pass identifies instructions that *would* compress if their virtual-register
operands were placed in `x8`–`x15`, and constrains those vregs to the `GPRC`
class via `MRI.constrainRegClass(V, &RISCV::GPRCRegClass)`. This is a
**correctness-preserving** narrowing: it only restricts where the allocator may
place a value, never changes program semantics. No spills, stack slots, or
constants are introduced — only register *class* constraints — so every variant
is a valid allocation by construction.

The catch: narrowing is not free. Forcing values into an 8-register window can
*increase* register pressure and cause spills, and the spill reload/store code
can outweigh the 2-byte savings. Whether narrowing a given function is a net win
depends on that function's pressure and call structure. The autotuner resolves
this empirically, per function.

---

## How to implement it

### 1. The pass (in-tree): `llvm/lib/Target/RISCV/RISCVASPSplit.cpp`

The pass is a pre-RA `MachineFunctionPass`, off by default. Relevant flags:

| Flag | Effect |
|---|---|
| `-riscv-asp-split` | enable the pass |
| `-riscv-asp-split-naive-gprc` | constrain **every** compression-candidate vreg to `GPRC` (no solver) |
| `-riscv-asp-split-skip-xcall` | within naive mode, **skip** any candidate whose live range crosses a call |
| `-riscv-asp-split-xcall-threshold=K` | soften the above: skip only if the range crosses **≥ K** calls (`K=1` = "any call") |
| `-riscv-asp-split-policy-file=<map>` | autotuner input: a per-function `name none\|all\|skipK` map |
| `-riscv-asp-split-dump-features` | emit a per-function static-feature vector (for studies) |

Core steps:

1. **Candidate detection** — `classify(MI, ST, Need)` returns true if `MI` is a
   compressible form (e.g. `LW`/`SW`/`LD`/`SD` with a `GPRC`-range immediate,
   `AND`/`OR`/`XOR`/`SUB`/… two-address ALU ops) and collects the vreg operands
   that must land in `x8`–`x15`.
2. **Narrowing** — insert those vregs into a `GPRCWanted` set and apply
   `constrainRegClass` in `applyActions`. The register allocator (RAGreedy) then
   honors the narrower class.
3. **Per-candidate refinement** (`skip-xcall`) — collect the function's call
   `SlotIndex`es once; for each candidate vreg, count how many calls its
   `LiveInterval` is live across (`LI.liveAt(callSlot)`) and skip narrowing it if
   the count ≥ threshold. In `ilp32`/`ilp32e`, `x10`–`x15` are caller-saved, so a
   narrowed value spanning calls forces save/restore churn — this lets the
   autotuner trial "narrow everything" vs "spare the call-crossers."
4. **Per-function policy** (`policy-file`) — `loadPolicyFile()` lazily reads a
   `StringMap<int>` of `funcname → {none, all, skipK}`. In
   `runOnMachineFunction` the per-function policy overrides the command-line
   default, so a single recompile embeds a different decision per function.
   `none` returns early (plain greedy); it never falls through to the solver.

### 2. The autotuner (out-of-tree): `autotune.sh` (+ `_poly`, `_mibench`)

The search is "compile-N-ways, keep the smallest per function," realized as a
build-time wrapper. For each translation unit:

1. Compile once per policy in `POLICIES` (default `none all skip skip2 skip3`)
   with `-ffunction-sections`, so every function lands in its own
   `.text.<fn>` section.
2. Measure each function's `.text` size with `llvm-nm --print-size`.
3. Pick the smallest policy per function (ties favor `none` → no churn) and
   write the `policy-file` map.
4. **Recompile once** with `-riscv-asp-split-policy-file=<map>` to produce the
   final object embedding the best-of-N choice. No object surgery: because
   allocation is per-function and `constrainRegClass` is correctness-safe, this
   single recompile reproduces the per-function winners exactly.
5. **Verify**: assert `final_size == predicted_min` per function (the harness
   reports a mismatch count, which is 0 in all runs).

Parallelism: translation units are independent, dispatched across
`JOBS` workers via `xargs -P` (≈10× wall-clock speedup on 18 cores). The
target ISA/ABI are env-overridable (`MARCH`/`MABI`).

```
# example: SPEC subset, standard 32-register rv32, 18-way parallel
MARCH=rv32i_zca_zcb_zcmp MABI=ilp32 \
  bash autotune.sh -o /tmp/objs 401.bzip2 429.mcf ...
```

### 3. Why search lives outside the compiler

The narrowing decision is made pre-RA, but its payoff (does the instruction
actually compress?) is only realized after register allocation. Evaluating N
policies therefore means running register allocation N times per function.
LLVM's pipeline is built around a single linear RA pass, so doing the search
in-process would require cloning `MachineFunction`s and re-running RAGreedy plus
its analysis stack per variant — invasive and serial. The build system instead
runs the full, correct pipeline once per variant and parallelizes across TUs for
free. This mirrors LLVM's established "mechanism in-tree, search out-of-tree"
pattern (PGO consumes a profile; BOLT optimizes post-link): the `policy-file` is
structurally a per-function profile the compiler consumes.

---

## Why it works

**The win** is a direct encoding-size effect: every candidate instruction whose
operands the allocator can keep in `x8`–`x15` shrinks 4 → 2 bytes.

**The cost** is spill pressure: `GPRC` is only 8 registers, so over-constraining
a hot function evicts other values to the stack, and the reload/store code can
exceed the compression savings.

The balance is governed by **register pressure**, and the cross-target data
(below) shows pressure is the *enemy* of narrowing, not its source. With the
register-count knob we can separate two distinct mechanisms:

- **Spill-free compression (dominant, scales with free registers).** When the
  register file has headroom, the allocator parks compressible values in
  `x8`–`x15` without evicting anything, banking the 2-byte forms at ~zero cost.
  This is most function-text by count.
- **Coordination / thrashing relief (minor, a high-pressure effect).** Under a
  cramped file, a *consistent* narrowing can also reduce greedy's
  eviction/split churn. This helps a handful of high-pressure integer kernels
  but shrinks as registers are added.

Because the spill-free effect dominates, the optimization is **more** effective
on the standard 32-register RISC-V than on embedded `rv32e` — the opposite of
the naive expectation that the cramped target benefits most.

The autotuner exists because no cheap static feature reliably predicts the
per-function balance (call density is the best single signal but captures only
~36% of the per-function oracle). Treating it as empirical per-function
autotuning over a tiny policy set sidesteps prediction entirely and is
regression-free by construction: `none` (baseline) is always in the trial set
and we take the per-function minimum, so no function — and no benchmark — can
grow.

---

## Results (per-function autotuner, `-Os`, regression-free, 0 verify mismatches)

Suite-level text reduction, two targets (same compressed ISA, `GPRC` = `x8`–`x15`
in both; only the register-file size differs):

| Suite | `rv32e` / `ilp32e` (16 GPRs) | `rv32i` / `ilp32` (32 GPRs) |
|---|---|---|
| SPEC CPU2006 subset (10) | −0.536% | **−1.089%** |
| MiBench | −0.424% | **−0.891%** |
| PolyBench/C 3.2 | −0.089% | **−1.988%** |

Two-regime split (per-benchmark, `rv32e` → `rv32i`):

- **Coordination kernels shrink with more registers:** bzip2 −3.79%→−3.60%,
  mcf −1.90%→−1.32%, rijndael −5.07%→−0.31% (collapses), patricia −3.83%→−1.45%.
- **Spill-free kernels grow with more registers:** lbm −0.33%→−3.73% (11×),
  sphinx3 −0.31%→−1.79%, hmmer −0.40%→−1.12%, blowfish 0→−1.80%, jpeg
  −0.36%→−1.66%.

Workload character (at `rv32e`): integer/control-flow code (SPEC, MiBench)
benefits; dense FP loop nests (PolyBench) barely do, because their arithmetic
uses the FP registers (not `GPRC`) and their integer addressing is low-pressure
and leaf — so the call-aware policies are inert there.

---

## Prior work

This idea is **not new in outline** — biasing register allocation toward the
compressed register subset is a known code-size technique — but the specific
combination here (correctness-safe class narrowing in LLVM + a regression-free
per-function autotuner + the register-pressure characterization) appears to be
novel.

- **Compression-aware register allocation for RISC-V (LibFirm).** The closest
  prior work: a KIT master's thesis (Stemmer/Grabow, 2021) implements
  compression-aware RA in the libFirm backend, biasing allocation toward the RVC
  register subset. It reports ~**4.2%** (Embench) and ~**5.7%** (SPEC CINT2000)
  text-segment reduction with <0.2% instruction-count growth. Same core idea,
  different compiler (libFirm, not LLVM), heuristic rather than autotuned, and it
  does not study the register-pressure dependence.
  <https://pp.ipd.kit.edu/uploads/publikationen/stemmergrabow21masterarbeit.pdf>
- **Unison — combinatorial register allocation (constraint programming).**
  Castañeda Lozano, Carlsson, Hjort Blindell, Schulte (TOPLAS 2019). A full
  constraint-programming model of global RA + scheduling, integrated with LLVM,
  scaling to ~1000-instruction functions; reports **0.8–3.9%** mean code-size
  reduction across Hexagon/ARM/MIPS. This is the methodological cousin of the
  broader ASP/solver line of this project (solver-based RA), though Unison does
  not specifically target RVC compression. <https://dl.acm.org/doi/10.1145/3332373>
- **ARM Thumb-2 narrow-encoding selection.** The established wisdom for 16-bit
  encodings (which similarly require `r0`–`r7`) is to *start unconstrained and
  decay to the compact encoding after RA*, so the encoding choice does not
  over-constrain the allocator. Our approach is the inverse (constrain before
  RA) but made safe by the per-function autotuner, which keeps narrowing only
  where it measured a win — recovering the same "don't hurt the constrained
  cases" guarantee empirically.
- **RVC background.** RISC-V's compressed extension yields ~25–30% static
  code-size reduction overall; ~50–60% of instructions are RVC-compressible
  (Waterman et al., UCB). The optimization here targets the residual: candidate
  instructions left uncompressed purely because their operands landed outside
  `x8`–`x15`. <https://www2.eecs.berkeley.edu/Pubs/TechRpts/2011/EECS-2011-63.html>
