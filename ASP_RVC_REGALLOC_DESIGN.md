# Compression-Aware Phase-1 GPRC Register Allocation for RISC-V (ASP/Clingo)

> Design and implementation notes for the `RISCVRVCRegAllocHints` pass and its
> companion Answer-Set-Programming model `regalloc_phase1_gprc.lp`.
>
> Status: experimental, gated behind `-riscv-asp-rvc-regalloc` (off by default),
> a no-op when LLVM is built without the in-tree Clingo project.

---

## 1. Motivation

The RISC-V **"C" (compressed) extension** encodes a subset of common
instructions in 16 bits instead of 32, halving their code size. Most of those
compressed forms can only address **8 of the 32 integer registers — `x8`–`x15`,
the `GPRC` register class** — because the compressed encoding has only 3 bits
per register field. A compressed encoding of a GPRC-requiring instruction is
therefore available **only when every register operand of that instruction lives
in `x8`–`x15`.**

GPRC is thus a *scarce, shared resource*, and deciding *which* values occupy it
is a global optimization problem: each GPRC register can hold only one value at
a time, values that are simultaneously live conflict, and some instructions
(two-address ALU ops) compress only when two of their operands share one
register. LLVM's greedy register allocator decides this **locally**, one live
range at a time, guided by per-instruction hints. This project replaces that
local decision, for the GPRC sub-problem, with a **global optimum** computed by
an Answer Set Programming (ASP) solver (Clingo).

### Relationship to the in-tree heuristic

`RISCVRegisterInfo::getRegAllocationHints()` already adds GPRC *copy/two-address
hints* for the reg-reg / reg-imm arithmetic ops. Our pass is the **optimal,
global counterpart** to that heuristic, and additionally covers a class the
in-tree heuristic ignores entirely:

| Compression class | In-tree hints? | Covered here? |
|---|---|---|
| Two-address ALU (`c.and/or/xor/sub/addw/subw`, `c.andi/srli/srai`, Zcb unary/`c.mul`) | yes | yes (global) |
| **Loads/stores** (`c.lw/sw/ld/sd`, Zcb byte/half) | **no** | **yes (new opportunity)** |

---

## 2. Architecture: two phases

```
                 ┌───────────────────────────────────────────────┐
   MIR (pre-RA)  │  RISCVRVCRegAllocHints  (this pass, phase 1)   │
   ───────────►  │   1. find GPRC compression candidates          │
                 │   2. emit an ASP program (facts + policy)      │
                 │   3. solve it with Clingo -> in_gprc(V,R)      │
                 │   4. MRI.addRegAllocationHint(V, phys(R))      │
                 └───────────────────────────────────────────────┘
                                      │  (hints only; MIR unchanged)
                                      ▼
                 ┌───────────────────────────────────────────────┐
   Greedy RA     │  Phase 2: LLVM greedy allocator + coalescer    │
                 │   honours the hints where pressure allows;     │
                 │   a candidate that does not get GPRC simply    │
                 │   stays 32-bit ("demoted") — no spill needed.  │
                 └───────────────────────────────────────────────┘
```

Key properties:

- **Hints are soft.** The pass changes nothing in the MIR; it only records
  preferences via `MRI.addRegAllocationHint`. Phase 2 honours them under
  pressure and ignores them otherwise.
- **No spill machinery.** "Demotion" (a candidate not getting GPRC) is just the
  absence of a win — the instruction keeps its legal 32-bit form.
- **Fail-safe.** A Clingo timeout, an `UNSAT` program, or a build without Clingo
  all yield *no hints*, leaving the allocator exactly as it was.

The pass is scheduled in `addPreRegAlloc()` (see `RISCVTargetMachine.cpp`),
requires `LiveIntervals`, and sets `AU.setPreservesAll()`.

---

## 3. The ASP model (`regalloc_phase1_gprc.lp`)

The model is split in two: a **static policy** (rules, identical to the
`kPhase1Prelude` string compiled into the pass) and **dynamic facts** (emitted
per `MachineFunction`). The standalone `.lp` file is kept in sync with the
embedded prelude and is runnable directly with `clingo` for debugging.

### 3.1 Input fact interface

| Fact | Meaning |
|---|---|
| `gprc_reg(R).` | `R` is a GPRC pool index (dense `0..7` → `x8..x15`). |
| `gprc_callee_saved(R).` | pool index `R` is callee-saved (`x8`/`x9`). |
| `cand(I).` | instruction `I` is a compression candidate (opcode/imm already checked). |
| `cand_saving(I, B).` | realizing `I` saves `B` bytes (always `2`). |
| `needs_gprc(I, V).` | operand vreg `V` of `I` must be in GPRC for `I` to compress. |
| `tied(V1, V2).` | two-address pair: `V1`,`V2` must share one register (or neither is placed). |
| `interfere(V1, V2).` | `V1`,`V2` are simultaneously live (emitted once, `V1<V2`). |
| `precolored(V, R).` | `V` is ABI-pinned to **GPRC** pool index `R` → fixed there. |
| `pinned_nongprc(V).` | `V` is ABI-pinned to a **non-GPRC** physreg → can never compress. |
| `cs_surcharge(S).` | one-time byte cost of touching *any* callee-saved GPRC reg. |
| `cs_per_reg(C).` | byte cost *per* callee-saved GPRC reg touched. |

Dense vreg ids and dense GPRC pool indices are assigned by the pass; they are
**not** MIR vreg numbers.

### 3.2 Rules, annotated

```prolog
% A vreg is a free GPRC-placement candidate unless it is ABI-pinned.
gprc_vreg(V) :- needs_gprc(_, V), not precolored(V, _), not pinned_nongprc(V).

% Symmetric closure of the two-address tie relation.
tied_sym(A, B) :- tied(A, B).
tied_sym(A, B) :- tied(B, A).

% Decision: each free candidate vreg gets at most one GPRC register.
{ in_gprc(V, R) : gprc_reg(R) } 1 :- gprc_vreg(V).
% ABI-pinned GPRC operands are fixed to their physical register.
in_gprc(V, R) :- precolored(V, R).
in_gprc(V)    :- in_gprc(V, _).

% Constraint: no two simultaneously-live vregs share a GPRC register.
:- in_gprc(V1, R), in_gprc(V2, R), interfere(V1, V2).

% Constraint: tied operands share one GPRC register, or neither is placed.
%  (A tied+interfering pair therefore can never be placed -> demoted, exactly
%   the cases that would have needed an extra coalescing copy.)
:- tied_sym(V1, V2), in_gprc(V1, R), not in_gprc(V2, R).

% Realization: a candidate compresses iff every required operand is in GPRC.
blocked(I)  :- needs_gprc(I, V), not in_gprc(V).
realized(I) :- cand(I), not blocked(I).

% Callee-saved accounting.
used_callee_saved(R) :- in_gprc(_, R), gprc_callee_saved(R).
any_callee_saved     :- used_callee_saved(_).

% Objective (single @2 priority level): maximize realized compression bytes
% minus the real spill cost of any callee-saved GPRC registers touched.
#maximize { B@2, realized, I : realized(I), cand_saving(I, B) }.
#minimize { S@2, surcharge : any_callee_saved, cs_surcharge(S) }.
#minimize { C@2, perreg, R : used_callee_saved(R), cs_per_reg(C) }.

#show in_gprc/2.
```

Two design subtleties worth calling out:

1. **Tied + interfering ⇒ automatically demoted.** The tie constraint forces
   tied operands onto the same register; the interference constraint forbids
   interfering operands from sharing one. A two-address candidate whose source
   does *not* die at the instruction (so rd interferes with rs1) thus becomes
   unsatisfiable to place — which is precisely the case the
   `TwoAddressInstruction` pass would resolve with an extra copy. The model
   demotes it for free, with no special-casing.

2. **Everything nets at one priority level.** The compression reward and the
   spill penalty share priority `@2`, so a candidate is realized only when its
   saving genuinely exceeds the spill it forces. (clingo combines a `#maximize`
   and `#minimize` at the same priority into one weighted sum, negating the
   minimize terms.) An earlier version used a lower-priority tie-break for
   callee-saved registers; that could never veto a compression and so walked
   straight into the profitability trap described in §4.

---

## 4. Feature-aware spill cost model

Touching a callee-saved GPRC register (`x8`/`x9`) forces the function to
save/restore it. Whether that is worth +2 bytes of compression depends entirely
on **how the target spills**, which RISC-V exposes as subtarget features:

| Spill mechanism | Prologue/epilogue | Marginal cost of one more callee-saved reg | `cs_surcharge` | `cs_per_reg` |
|---|---|---|---|---|
| **inline frame** (default) | `c.addi16sp` + `c.sdsp`/`c.ldsp` pair per reg | ~4 bytes each | `4` | `4` |
| **`+save-restore`** | `call t0,__riscv_save_N` / `tail __riscv_restore_N` (shared millicode) | ~0 (amortized in the shared routine) | `8` | `0` |
| **`+zcmp`** | `cm.push` / `cm.popret` (single 16-bit insns covering a register list) | ~0 (one push list covers the whole range) | `2` | `0` |

The pass selects the weights from `ST.hasStdExtZcmp()` and
`ST.enableSaveRestore()` and emits them as `cs_surcharge/1` and `cs_per_reg/1`.
The consequence is that the **same model makes different, correct decisions per
target**:

- Under an **inline frame**, the first callee-saved register costs
  `surcharge(4) + per_reg(4) = 8` bytes — more than the 4 bytes two compressions
  would save — so the solver prefers to leave `x8`/`x9` alone and only spends
  them when several compressions ride on them.
- Under **Zcmp**, a single `cm.push {ra, s0-s1}` brings in `x8` *and* `x9` for a
  ~2-byte one-time cost and `0` marginal cost, so the solver spends them freely.

> **Why this matters (the profitability trap).** Before this model existed, the
> objective maximized the *count* of compressed instructions and treated
> callee-saved registers with a low-priority tie-break. On a high-pressure ALU
> chain the solver happily grabbed `x8`/`x9` to win one extra `c.or` (+2 bytes)
> while silently adding an inline frame (`addi16sp` + two `sd`/`ld` pairs,
> ~12 bytes) — a **net +10-byte regression** dressed up as "more compressed
> instructions." Pricing the spill at its true, feature-dependent cost fixes
> this.

This is also where the global allocator demonstrably **beats** greedy: see §7.

---

## 5. ABI-precolor modeling

At the point this pass runs (pre-RA) virtual registers are not yet assigned
physical registers, but many are effectively pinned by the ABI: function
arguments arrive in `a0`–`a7`, return values leave in `a0`/`a1`, call operands
occupy specific argument registers. In the MIR these appear as `COPY`
instructions between a vreg and a physical GPR, and the register **coalescer**
will almost always keep the vreg in that physical register.

If the model does not know this, it will plan placements the coalescer simply
undoes. Concretely: a function with seven pointer arguments has its 7th pointer
in `a6`/`x16`, which is **not** GPRC. A naïve model treats that pointer as a
freely-placeable vreg, "decides" to compress stores through it by planning it
into a GPRC register, and emits hints accordingly — but the coalescer keeps it
in `x16`, the stores never compress, and the now-conflicting hints **mislead
greedy into a worse allocation than no hints at all** (observed: 4 compressed
loads vs. the zero-hint baseline's 6).

### Detection

The pass scans every `COPY` whose other side is a physical GPR and records a
vreg → physreg affinity. A vreg with conflicting affinities (copied to two
different physregs) is left unpinned. For each candidate operand that has an
affinity it emits:

- `precolored(V, poolIdx)` if the physreg is in the GPRC pool (`x8`–`x15`) — the
  operand is fixed there and still participates in interference;
- `pinned_nongprc(V)` otherwise — the operand can never be GPRC, so any candidate
  requiring it is `blocked`.

With this, the seven-pointer example correctly recognizes `q`→`x16` as
`pinned_nongprc`, demotes the impossible stores, and falls back to the feasible
6-load plan that matches what greedy already does — **eliminating the
regression.**

---

## 6. The LLVM pass (`RISCVRVCRegAllocHints.cpp`)

### 6.1 Candidate classification — `classifyCompressible`

Mirrors the authoritative opcode/immediate tests in
`RISCVRegisterInfo::getRegAllocationHints`, extended with loads/stores. Returns
a `CandKind`:

| `CandKind` | Operands needing GPRC | Tie | Examples |
|---|---|---|---|
| `CK_LoadStore` | data(op0), base(op1) | none | `LW`/`LD`/`SW`/`SD`; Zcb `LBU`/`LH`/`LHU`/`SB`/`SH` |
| `CK_TwoOp` | rd(op0), rs1(op1) | rd~rs1 | `ANDI` (`isInt<6>` or Zcb `==255`), `SRAI`/`SRLI`, Zcb `SEXT_*`/`ZEXT_H_*`/`ADD_UW`/`XORI` |
| `CK_RR3` | rd, rs1, rs2 | rd~dying source | `AND`/`OR`/`XOR`/`ADDW` (commutable), `SUB`/`SUBW`, Zcb `MUL` (commutable) |

Immediate ranges use the same scaled predicates as the assembler
(`isShiftedUInt<5,2>` for word loads/stores, `<5,3>` for doubles, etc.). Zcb
forms are gated on `ST.hasStdExtZcb()`; the whole pass is gated on
`ST.hasStdExtZca()`.

### 6.2 Tie selection for `CK_RR3`

A reg-reg ALU op compresses only as `rd = rd op rs`, i.e. with `rd` tied to one
source. The pass ties `rd` to the source that **dies** at the instruction
(checked via `LiveIntervals::overlaps`), matching the commutation the
`TwoAddressInstruction` pass will choose: `rs1` if `rd` doesn't overlap it, else
`rs2` for a commutable op. If neither source dies, it ties `rd~rs1` anyway —
which (being interfering) makes the candidate unsatisfiable to place, the
correct "needs a copy, not free" outcome.

### 6.3 Fact emission, solve, and hint application

1. Build a dense `vreg → id` map (`getId`) as candidates are discovered.
2. Scan COPYs to build the ABI-affinity map (§5).
3. For each candidate emit `cand`, `cand_saving(=2)`, `needs_gprc`, and `tied`.
4. Emit the GPRC pool (`gprc_reg`, `gprc_callee_saved`), the feature-aware
   `cs_surcharge`/`cs_per_reg` (§4), the ABI-pin facts (§5), and pairwise
   `interfere` for overlapping candidate vregs.
5. Prepend `kPhase1Prelude`, hand the program to `runPhase1Clingo`.
6. Apply one de-duplicated `addRegAllocationHint(V, phys(R))` per `in_gprc(V,R)`
   atom in the optimal model.

### 6.4 In-process Clingo — `runPhase1Clingo`

Uses the libclingo C API: `clingo_control_new({"--opt-mode=opt"})`, `add`,
`ground`, then `solve` in **yield mode**. The yield loop *resumes to
exhaustion*, keeping the **last** model — which under `--opt-mode=opt` is the
optimum. A watchdog `std::thread` calls `clingo_control_interrupt` after
`-riscv-asp-rvc-time-limit` seconds (default 10); an interrupted or empty solve
returns "no hints."

> Implementation note discovered during validation: the loop **must** drain all
> yielded models. `--opt-mode=opt` emits a stream of strictly-improving models
> (e.g. `-8, -10, -12, -20`); stopping at the first yields a valid-but-suboptimal
> assignment. Keeping `Latest` across the whole loop is what makes the result
> globally optimal.

---

## 7. Empirical results

Built `Debug+asserts`, `-mtriple=riscv64`. Compressed-instruction counts and
total `.text` size measured by disassembling the linked object (2-byte encodings
= compressed, 4-byte = full).

### 7.1 No regressions (precolor modeling)

| Test | baseline | ASP |
|---|---|---|
| `rvc_alu` (two-address chain) | 5 comp / 18 B | 5 comp / 18 B |
| `rvc_ls` (4 loads + 4 stores) | 9 comp / 18 B | 9 comp / 18 B |
| `rvc_pressure` (10-mul + ALU chain) | 5 comp / 70 B | 5 comp / 70 B |
| `rvc_ls_pressure` (7-pointer gather/scatter) | 7 comp / 38 B | 7 comp / 38 B |

`rvc_pressure` previously regressed to 80 B (spurious inline frame) and
`rvc_ls_pressure` to 42 B (infeasible store plan); the cost model and precolor
modeling respectively eliminate both.

### 7.2 A genuine divergence where ASP beats greedy

`gatherstore`: 10 values loaded through a GPRC base pointer, all live
simultaneously, then stored through a second GPRC base. Each value placed in
GPRC wins **two** compressions (`c.lw` + `c.sw`); LLVM emits no load/store hints,
so the baseline's compression is incidental to greedy's register choices.

| Config | baseline | ASP | Δ |
|---|---|---|---|
| inline (`+c,+m`) | 11 comp / 62 B | 11 comp / 62 B | tie |
| `+save-restore` | 11 comp / 62 B | 11 comp / 62 B | tie |
| **`+zcmp`** | 11 comp / 62 B | **16 comp / 56 B** | **+5 comp, −6 B** |

Under Zcmp the solver recognizes that `x8`/`x9` are nearly free (one
`cm.push {ra, s0-s1}`), and that loads/stores — which greedy never hints, so it
**leaves `x8`/`x9` entirely unused** (verified: 0 uses in the baseline) — can be
compressed by parking two more values there. It wins 4 extra load/store
compressions for a ~2-byte frame cost. Under an inline frame the *same* model
correctly **declines** (the frame would cost more than it saves) and ties the
baseline rather than regressing. This is the thesis result in miniature: a
**global, feature-aware** allocator harvesting compression headroom (un-hinted
loads/stores × cheap-under-Zcmp callee-saved registers) that a local greedy
heuristic structurally cannot see.

---

## 8. Build and usage

### Build integration (`lib/Target/RISCV/CMakeLists.txt`)

`RISCVRVCRegAllocHints.cpp` is added to `LLVMRISCVCodeGen`. When the in-tree
`clingo` project is enabled, the target gets `-DLLVM_PBQP_HAVE_CLINGO`, the
libclingo include dir, and links `libclingo.a`/`libgringo.a`/`libreify.a`/
`libclasp`/`libpotassco`. Without it, the file compiles to a one-time warning
no-op.

### Flags

| Flag | Default | Meaning |
|---|---|---|
| `-riscv-asp-rvc-regalloc` | off | enable the ASP phase-1 GPRC allocator |
| `-riscv-asp-rvc-time-limit=N` | `10` | Clingo wall-clock budget in seconds |
| `-debug-only=riscv-asp-rvc` | — | dump the generated ASP program and emitted hints |

### Debugging the model standalone

The generated program (from `-debug-only=riscv-asp-rvc`) can be split into the
static policy and the facts; the facts can be appended to
`regalloc_phase1_gprc.lp` and run directly:

```
clingo regalloc_phase1_gprc.lp facts.lp --opt-mode=opt
```

which prints the `in_gprc/2`, `realized/1`, and `demoted/1` of the optimum.

---

## 9. Known limitations & future work

- **Soft hints are a lossy channel.** Even an optimal, feasible assignment is
  communicated only as per-vreg preferences; under heavy pressure greedy can
  still deviate. Precolor modeling avoids the pathological case, but a binding
  phase-1→phase-2 coupling (e.g. honoring the assignment as a hard constraint)
  would make the win robust.
- **Microbenchmarks only.** The §7 results are constructed cases. Validation on
  a full benchmark suite (e.g. embench-iot, compiled `-Os`) is future work.
- **COPY-chain coalescing is not modeled.** A value flowing through a chain of
  copies into a fixed register (e.g. an accumulator that must end in the return
  register `a0`) is only partially captured by direct-COPY affinity; longer
  chains can still let the model pick a locally-fine-but-globally-worse register.
- **Floating-point compressed loads/stores** (`c.flw`/`c.fld`) would need a
  separate `FPR32C`/`FPR64C` pool; deliberately out of scope here.
- **GPRPair / subregisters** are conservatively ignored, mirroring the in-tree
  heuristic's `TODO`.

---

## 10. File-by-file change summary

| File | Change |
|---|---|
| `llvm/lib/Target/RISCV/RISCVRVCRegAllocHints.cpp` | The pass: candidate classification, tie selection, ABI-affinity scan, feature-aware cost emission, in-process Clingo solve, hint application. (header `Modelling notes` and `kPhase1Prelude` document the model inline.) |
| `llvm/lib/Target/RISCV/RISCV.h` | Declares `createRISCVRVCRegAllocHintsPass` / `initializeRISCVRVCRegAllocHintsPass`. |
| `llvm/lib/Target/RISCV/RISCVTargetMachine.cpp` | Registers the pass and schedules it in `addPreRegAlloc()`. |
| `llvm/lib/Target/RISCV/CMakeLists.txt` | Adds the source and the optional Clingo link/define. |
| `ASP-PBQP-regalloc/regalloc_phase1_gprc.lp` | Standalone, runnable mirror of the embedded policy: precolor/pinned guards, feature-aware spill-cost facts, single-level netted objective. |
