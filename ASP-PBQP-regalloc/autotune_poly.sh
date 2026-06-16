#!/bin/bash
# Per-kernel compression autotuner for the PolyBench/C 3.2 suite.
#
# PolyBench is ~30 single-file compute kernels (dense loop nests).  Each kernel
# .c is its own translation unit / "benchmark"; at -Os the static init/kernel/
# print helpers inline into main, so we measure total .text per TU.  Same engine
# as autotune.sh: compile each TU under every policy in POLICIES, pick the
# smallest .text per function, write a policy map, recompile once and verify.
#
# Env: POLICIES="all skip skip2 skip3"  JOBS=<n>
#   bash ASP-PBQP-regalloc/autotune_poly.sh [-o OUTDIR]
# Per-function CSV on stdout (bench = kernel name), suite summary on stderr.

ROOT=/Users/jupiterbradley/Documents/University/msc-thesis/clingo-regalloc/llvm-project-ASP-regalloc
CLANG=$ROOT/build/bin/clang
NM=$ROOT/build/bin/llvm-nm
INC=/opt/homebrew/Cellar/riscv-gnu-toolchain/main/riscv64-unknown-elf/include
POLY=$ROOT/polybench-c-3.2
UTIL=$POLY/utilities
COMMONC="--target=riscv32-unknown-elf -mabi=${MABI:-ilp32e} -isystem $INC -march=${MARCH:-rv32e_zca_zcb_zcmp} -Os -w -ffunction-sections -fdata-sections -Wno-implicit-function-declaration -Wno-implicit-int -Wno-return-type"

read -r -a POLS <<< "${POLICIES:-all skip skip2 skip3}"

polflags(){
  case "$1" in
    all)   echo "-mllvm -riscv-asp-split -mllvm -riscv-asp-split-naive-gprc";;
    skip)  echo "-mllvm -riscv-asp-split -mllvm -riscv-asp-split-naive-gprc -mllvm -riscv-asp-split-skip-xcall";;
    skip*) echo "-mllvm -riscv-asp-split -mllvm -riscv-asp-split-naive-gprc -mllvm -riscv-asp-split-skip-xcall -mllvm -riscv-asp-split-xcall-threshold=${1#skip}";;
  esac
}
syms(){ "$NM" --print-size --defined-only --radix=d "$1" 2>/dev/null | awk '$3=="t"||$3=="T"{print $4, $2+0}' | sort -k1,1; }

# ---- worker: process one kernel TU -------------------------------------------
# argv: --worker <resultsdir> <outdir|-> <file>
if [ "$1" = "--worker" ]; then
  RESULTS="$2"; OUTDIR="$3"; f="$4"
  B=$(basename "$f" .c)                         # kernel name = benchmark name
  KDIR=$(dirname "$f")
  CC=($CLANG $COMMONC -I "$UTIL" -I "$KDIR")
  W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
  "${CC[@]}" -c "$f" -o "$W/none.o" 2>/dev/null || exit 0
  : > "$W/sizes.txt"
  syms "$W/none.o" | awk '{print $1, "none", $2}' >> "$W/sizes.txt"
  for P in "${POLS[@]}"; do
    "${CC[@]}" $(polflags "$P") -c "$f" -o "$W/p.o" 2>/dev/null || exit 0
    syms "$W/p.o" | awk -v t="$P" '{print $1, t, $2}' >> "$W/sizes.txt"
  done
  : > "$W/policy.txt"
  awk '{
    name=$1; tag=$2; sz=$3;
    if (tag=="none") base[name]=sz;
    if (!(name in bestsz) || sz < bestsz[name]) { bestsz[name]=sz; bestpol[name]=tag }
  }
  END {
    for (n in bestsz) {
      if (bestpol[n]!="none") print n, bestpol[n] > "'"$W/policy.txt"'"
      print n, base[n], bestsz[n], bestpol[n]
    }
  }' "$W/sizes.txt" | sort -k1,1 > "$W/decided.txt"
  "${CC[@]}" -mllvm -riscv-asp-split -mllvm -riscv-asp-split-policy-file="$W/policy.txt" \
     -c "$f" -o "$W/final.o" 2>/dev/null || exit 0
  [ "$OUTDIR" != "-" ] && cp "$W/final.o" "$OUTDIR/$B.o"
  syms "$W/final.o" > "$W/fin.txt"
  out=$(mktemp "$RESULTS/tu.XXXXXX")
  join "$W/decided.txt" "$W/fin.txt" 2>/dev/null | awk -v bench="$B" '
    { printf "%s,%s,%d,%d,%s,%d\n", bench, $1, $2, $3, $4, $5 }' > "$out"
  exit 0
fi

# ---- driver ------------------------------------------------------------------
OUTDIR="-"
[ "$1" = "-o" ] && { OUTDIR="$2"; shift 2; mkdir -p "$OUTDIR"; }
JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}
RESULTS=$(mktemp -d); trap 'rm -rf "$RESULTS"' EXIT

# All kernel .c files except the shared utilities/templates.
find "$POLY" -name '*.c' ! -path "$UTIL/*" -print0 \
  | xargs -0 -P "$JOBS" -I {} bash "$0" --worker "$RESULTS" "$OUTDIR" {}

echo "bench,func,base,best,policy,final"
cat "$RESULTS"/tu.* 2>/dev/null

{
  echo "================ PolyBench autotuner suite summary ================"
  echo "trial policies: none ${POLS[*]}   (JOBS=$JOBS)"
  cat "$RESULTS"/tu.* 2>/dev/null | awk -F, '
    { tb+=$3; to+=$4; tf+=$6; if ($6!=$4) mm++; cnt[$5]++ }
    END {
      printf "kernels x funcs: %d rows\n", NR
      printf "total base text:   %d\n", tb
      printf "total final text:  %d   (delta %+d, %.3f%%)\n", tf, tf-tb, (tf-tb)/tb*100
      printf "predicted oracle:  %d   (delta %+d)\n", to, to-tb
      printf "verify mismatches: %d\n", mm
      printf "policy picks:"; for (p in cnt) printf " %s=%d", p, cnt[p]; print ""
    }'
} >&2
