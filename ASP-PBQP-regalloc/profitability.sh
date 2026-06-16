#!/bin/bash
# Per-function profitability study for naive GPRC narrowing.
#
# For every function in the chosen benchmarks, emit a row:
#   bench,func,base_bytes,delta_bytes,maxlt,pts,calls,cands,candrefs,blocks
# where delta_bytes is the ORACLE: that function's size change from narrowing
# (constrain-all, gate 0) vs plain greedy.  Because allocation is per-function
# and constrainRegClass is correctness-safe, each function's delta is its own
# isolated narrowing benefit -- a clean ground-truth label.  An external
# analysis can then test which features predict delta<0 (profitable).
#
#   bash ASP-PBQP-regalloc/profitability.sh > ~/profit.csv
# then analyze ~/profit.csv.

ROOT=/Users/jupiterbradley/Documents/University/msc-thesis/clingo-regalloc/llvm-project-ASP-regalloc
CLANG=$ROOT/build/bin/clang
NM=$ROOT/build/bin/llvm-nm
INC=/opt/homebrew/Cellar/riscv-gnu-toolchain/main/riscv64-unknown-elf/include
SPEC="/Users/jupiterbradley/Virtual Machines.localized/ubuntu-shared-folder/reimplementing-compression-aware-register-allocator/app/cpu2006/benchspec/CPU2006"
CXXINC=(-isystem "$INC/c++/13.2.0" -isystem "$INC/c++/13.2.0/riscv64-unknown-elf" -std=gnu++98)
COMMONC="--target=riscv32-unknown-elf -mabi=ilp32e -isystem $INC -march=rv32e_zca_zcb_zcmp -Os -w -DSPEC_CPU -DSPEC_CPU_LINUX -DNDEBUG -Wno-implicit-function-declaration -Wno-implicit-int -Wno-return-type -Wno-int-conversion"
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

BENCHES=("$@")
[ ${#BENCHES[@]} -eq 0 ] && BENCHES=(401.bzip2 429.mcf 444.namd 458.sjeng 445.gobmk 464.h264ref 456.hmmer 482.sphinx3)

# func -> size (text symbols only) from llvm-nm; --radix=d gives a decimal size
# column (avoids gawk-only strtonum -- macOS ships BSD awk).
syms(){ "$NM" --print-size --defined-only --radix=d "$1" 2>/dev/null | awk '$3=="t"||$3=="T"{print $4, $2+0}'; }

echo "bench,func,base_bytes,delta_bytes,maxlt,pts,calls,cands,candrefs,blocks,ncand,xcall,spanblk,clivelen"
for B in "${BENCHES[@]}"; do
  SRC="$SPEC/$B/src"; [ -d "$SRC" ] || continue
  EXTRA=(); [ "$B" = "433.milc" ] && EXTRA=(-DFN)
  INCDIRS=(); while IFS= read -r d; do INCDIRS+=(-I "$d"); done < <(find "$SRC" -type d)
  while IFS= read -r f; do
    lang=(); case "${f##*.}" in cpp|cc|C) lang=("${CXXINC[@]}");; esac
    base="$WORK/b.o"; narr="$WORK/n.o"; feat="$WORK/feat.txt"
    $CLANG $COMMONC "${EXTRA[@]}" "${lang[@]}" "${INCDIRS[@]}" -c "$f" -o "$base" 2>/dev/null || continue
    $CLANG $COMMONC "${EXTRA[@]}" "${lang[@]}" "${INCDIRS[@]}" \
       -mllvm -riscv-asp-split -mllvm -riscv-asp-split-naive-gprc \
       -mllvm -riscv-asp-split-dump-features -c "$f" -o "$narr" 2>"$feat" || continue
    # per-function text-symbol sizes, sorted by name; joined to "name base narr"
    syms "$base" | sort -k1,1 > "$WORK/bs.txt"
    syms "$narr" | sort -k1,1 > "$WORK/ns.txt"
    join "$WORK/bs.txt" "$WORK/ns.txt" > "$WORK/sz.txt"   # name base narr
    # features keyed by func (from the narrowed compile's stderr)
    awk '/^\[asp-feat\]/{f=$2; for(i=4;i<=NF;i++){split($i,kv,"="); F[f"_"kv[1]]=kv[2]} fn[f]=1}
         END{for(k in fn) print k, F[k"_maxlt"], F[k"_pts"], F[k"_calls"], F[k"_cands"], F[k"_candrefs"], F[k"_blocks"], F[k"_ncand"], F[k"_xcall"], F[k"_spanblk"], F[k"_clivelen"]}' "$feat" > "$WORK/ft.txt"
    # merge sizes + features (NR==FNR: first file=sizes, second=features)
    awk -v bench="$B" '
      NR==FNR { base[$1]=$2; narr[$1]=$3; next }
      ($1 in base) && NF>=11 {
        printf "%s,%s,%d,%d,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
               bench, $1, base[$1], narr[$1]-base[$1], $2, $3, $4, $5, $6, $7, $8, $9, $10, $11
      }
    ' "$WORK/sz.txt" "$WORK/ft.txt"
  done < <(find "$SRC" \( -name '*.c' -o -name '*.cpp' -o -name '*.cc' -o -name '*.C' \))
done
