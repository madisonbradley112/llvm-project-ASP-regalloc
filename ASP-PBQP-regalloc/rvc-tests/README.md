# RVC GPRC-allocation test inputs

Hand-written LLVM IR exercising the ASP phase-1 GPRC allocator
(`-riscv-asp-rvc-regalloc`). See `../../ASP_RVC_REGALLOC_DESIGN.md` §7 for the
full results discussion.

| File | What it exercises | Role in §7 |
|---|---|---|
| `rvc_alu.ll` | two-address ALU chain (`c.and/or/xor/sub`) | no-regression |
| `rvc_ls.ll` | 4 loads + 4 stores through two pointers | no-regression |
| `rvc_pressure.ll` | 10 independent muls + two-address ALU chain (high GPRC pressure) | callee-saved profitability (no spurious inline frame) |
| `rvc_ls_pressure.ll` | 7-pointer gather/scatter (7th base is `x16`, non-GPRC) | precolor modeling (no infeasible-store regression) |
| `rvc_gatherstore.ll` | 10 values loaded/stored through two GPRC bases, all live | **divergence: ASP beats greedy under `+zcmp`** |

## Reproducing

The pass is gated and off by default; compare baseline vs. ASP and count
compressed (2-byte) vs. full (4-byte) encodings:

```sh
LLC=path/to/build/bin/llc
MC=path/to/build/bin/llvm-mc
OD=path/to/build/bin/llvm-objdump

count() {            # $1 = mattr, $2 = extra llc flags, $3 = file
  $LLC -mtriple=riscv64 -mattr="$1" $2 "$3" -o /tmp/t.s
  $MC  -triple riscv64  -mattr="$1" -filetype=obj /tmp/t.s -o /tmp/t.o
  $OD -d /tmp/t.o | awk '/^[ ]+[0-9a-f]+:/{h=$2;
    if(length(h)==4){c++;b+=2} else if(length(h)==8){w++;b+=4}}
    END{printf "comp:%d full:%d text:%d\n", c, w, b}'
}

# The headline divergence (expect 11/62 baseline vs 16/56 ASP):
count "+m,+zca,+zcb,+zcmp" ""                        rvc_gatherstore.ll
count "+m,+zca,+zcb,+zcmp" "-riscv-asp-rvc-regalloc" rvc_gatherstore.ll
```

To inspect the generated ASP program and the hints, add
`-debug-only=riscv-asp-rvc` (requires an assertions build).
