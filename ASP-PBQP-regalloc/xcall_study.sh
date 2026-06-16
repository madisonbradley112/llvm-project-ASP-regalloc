#!/bin/bash
# Per-candidate narrowing study.  Same structure as profitability.sh, but the
# narrowed compile uses -riscv-asp-split-skip-xcall: constrain only candidate
# vregs whose live range does NOT cross a call.  Emits per-function:
#   bench,func,base_bytes,delta_skipxcall
# Join against profit2.csv (delta_bytes = constrain-all) to compare policies.
#   bash ASP-PBQP-regalloc/xcall_study.sh <benches...> > ~/xcall.csv

ROOT=/Users/jupiterbradley/Documents/University/msc-thesis/clingo-regalloc/llvm-project-ASP-regalloc
CLANG=$ROOT/build/bin/clang
NM=$ROOT/build/bin/llvm-nm
INC=/opt/homebrew/Cellar/riscv-gnu-toolchain/main/riscv64-unknown-elf/include
SPEC="/Users/jupiterbradley/Virtual Machines.localized/ubuntu-shared-folder/reimplementing-compression-aware-register-allocator/app/cpu2006/benchspec/CPU2006"
CXXINC=(-isystem "$INC/c++/13.2.0" -isystem "$INC/c++/13.2.0/riscv64-unknown-elf" -std=gnu++98)
COMMONC="--target=riscv32-unknown-elf -mabi=ilp32e -isystem $INC -march=rv32e_zca_zcb_zcmp -Os -w -DSPEC_CPU -DSPEC_CPU_LINUX -DNDEBUG -Wno-implicit-function-declaration -Wno-implicit-int -Wno-return-type -Wno-int-conversion"
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

BENCHES=("$@")
[ ${#BENCHES[@]} -eq 0 ] && BENCHES=(401.bzip2 429.mcf 444.namd 458.sjeng 445.gobmk 464.h264ref 456.hmmer 482.sphinx3 470.lbm 473.astar)

syms(){ "$NM" --print-size --defined-only --radix=d "$1" 2>/dev/null | awk '$3=="t"||$3=="T"{print $4, $2+0}'; }

echo "bench,func,base_bytes,delta_skipxcall"
for B in "${BENCHES[@]}"; do
  SRC="$SPEC/$B/src"; [ -d "$SRC" ] || continue
  EXTRA=(); [ "$B" = "433.milc" ] && EXTRA=(-DFN)
  INCDIRS=(); while IFS= read -r d; do INCDIRS+=(-I "$d"); done < <(find "$SRC" -type d)
  while IFS= read -r f; do
    lang=(); case "${f##*.}" in cpp|cc|C) lang=("${CXXINC[@]}");; esac
    base="$WORK/b.o"; narr="$WORK/n.o"
    $CLANG $COMMONC "${EXTRA[@]}" "${lang[@]}" "${INCDIRS[@]}" -c "$f" -o "$base" 2>/dev/null || continue
    $CLANG $COMMONC "${EXTRA[@]}" "${lang[@]}" "${INCDIRS[@]}" \
       -mllvm -riscv-asp-split -mllvm -riscv-asp-split-naive-gprc \
       -mllvm -riscv-asp-split-skip-xcall -c "$f" -o "$narr" 2>/dev/null || continue
    syms "$base" | sort -k1,1 > "$WORK/bs.txt"
    syms "$narr" | sort -k1,1 > "$WORK/ns.txt"
    join "$WORK/bs.txt" "$WORK/ns.txt" | awk -v bench="$B" '{printf "%s,%s,%d,%d\n", bench, $1, $2, $3-$2}'
  done < <(find "$SRC" \( -name '*.c' -o -name '*.cpp' -o -name '*.cc' -o -name '*.C' \))
done
