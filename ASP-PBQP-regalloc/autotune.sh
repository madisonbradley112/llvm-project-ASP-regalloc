#!/bin/bash
# Per-function compression autotuner (parallel).
#
# For each translation unit, compile it under each narrowing policy in POLICIES
# (none / all=constrain-all / skipK=skip candidates crossing >=K calls) with
# -ffunction-sections, measure every function's .text size, pick the smallest
# policy per function, write a policy map, and recompile ONCE with
# -riscv-asp-split-policy-file=<map> so a single correct object embeds the
# best-of-N choice per function.
#
# No object surgery: register allocation is per-function and constrainRegClass
# is correctness-safe, so the final recompile reproduces exactly the per-function
# winners (verified).  Cost: 1 baseline + N policy + 1 final compile per TU.
#
# Parallelism: translation units are independent, so they are dispatched across
# JOBS workers via `xargs -P` (each worker re-invokes this script in --worker
# mode with its own temp dir).  Per-function rows are written to per-TU files
# and concatenated at the end; the suite summary is computed from the combined
# CSV (no cross-process shell-variable accumulation).
#
# Env: POLICIES="all skip skip2 skip3" (trial set; 'none' always implicit)
#      JOBS=<n>  (parallel workers; default = CPU count)
#
#   bash ASP-PBQP-regalloc/autotune.sh [-o OUTDIR] <benches...>
# Emits a per-function CSV on stdout and a suite summary on stderr.

ROOT=/Users/jupiterbradley/Documents/University/msc-thesis/clingo-regalloc/llvm-project-ASP-regalloc
CLANG=$ROOT/build/bin/clang
NM=$ROOT/build/bin/llvm-nm
INC=/opt/homebrew/Cellar/riscv-gnu-toolchain/main/riscv64-unknown-elf/include
SPEC="/Users/jupiterbradley/Virtual Machines.localized/ubuntu-shared-folder/reimplementing-compression-aware-register-allocator/app/cpu2006/benchspec/CPU2006"
CXXINC=(-isystem "$INC/c++/13.2.0" -isystem "$INC/c++/13.2.0/riscv64-unknown-elf" -std=gnu++98)
COMMONC="--target=riscv32-unknown-elf -mabi=${MABI:-ilp32e} -isystem $INC -march=${MARCH:-rv32e_zca_zcb_zcmp} -Os -w -ffunction-sections -fdata-sections -DSPEC_CPU -DSPEC_CPU_LINUX -DNDEBUG -Wno-implicit-function-declaration -Wno-implicit-int -Wno-return-type -Wno-int-conversion"

# Trial policy set (baseline 'none' is always implicit and measured separately).
read -r -a POLS <<< "${POLICIES:-all skip skip2 skip3}"

# Map a policy token -> the -mllvm flags that realize it.
polflags(){
  case "$1" in
    all)   echo "-mllvm -riscv-asp-split -mllvm -riscv-asp-split-naive-gprc";;
    skip)  echo "-mllvm -riscv-asp-split -mllvm -riscv-asp-split-naive-gprc -mllvm -riscv-asp-split-skip-xcall";;
    skip*) echo "-mllvm -riscv-asp-split -mllvm -riscv-asp-split-naive-gprc -mllvm -riscv-asp-split-skip-xcall -mllvm -riscv-asp-split-xcall-threshold=${1#skip}";;
  esac
}

syms(){ "$NM" --print-size --defined-only --radix=d "$1" 2>/dev/null | awk '$3=="t"||$3=="T"{print $4, $2+0}' | sort -k1,1; }

# ---- worker: process a single translation unit -------------------------------
# argv: --worker <bench> <resultsdir> <outdir|-> <file>
if [ "$1" = "--worker" ]; then
  B="$2"; RESULTS="$3"; OUTDIR="$4"; f="$5"
  SRC="$SPEC/$B/src"
  EXTRA=(); [ "$B" = "433.milc" ] && EXTRA=(-DFN)
  INCDIRS=(); while IFS= read -r d; do INCDIRS+=(-I "$d"); done < <(find "$SRC" -type d)
  lang=(); case "${f##*.}" in cpp|cc|C) lang=("${CXXINC[@]}");; esac
  CC=($CLANG $COMMONC "${EXTRA[@]}" "${lang[@]}" "${INCDIRS[@]}")
  W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
  # 1) baseline + every trial policy into one tagged table: "name tag size"
  "${CC[@]}" -c "$f" -o "$W/none.o" 2>/dev/null || exit 0
  : > "$W/sizes.txt"
  syms "$W/none.o" | awk '{print $1, "none", $2}' >> "$W/sizes.txt"
  for P in "${POLS[@]}"; do
    "${CC[@]}" $(polflags "$P") -c "$f" -o "$W/p.o" 2>/dev/null || exit 0
    syms "$W/p.o" | awk -v t="$P" '{print $1, t, $2}' >> "$W/sizes.txt"
  done
  # 2) per function pick the smallest; ties favour 'none' (only update on <).
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
  # 3) final recompile honoring the policy map
  "${CC[@]}" -mllvm -riscv-asp-split \
     -mllvm -riscv-asp-split-policy-file="$W/policy.txt" \
     -c "$f" -o "$W/final.o" 2>/dev/null || exit 0
  [ "$OUTDIR" != "-" ] && cp "$W/final.o" "$OUTDIR/$(echo "$B-$(basename "$f")" | tr / _).o"
  syms "$W/final.o" > "$W/fin.txt"
  # 4) emit per-func rows (verification of final==best happens in the summary)
  out=$(mktemp "$RESULTS/tu.XXXXXX")
  join "$W/decided.txt" "$W/fin.txt" 2>/dev/null | awk -v bench="$B" '
    { printf "%s,%s,%d,%d,%s,%d\n", bench, $1, $2, $3, $4, $5 }' > "$out"
  exit 0
fi

# ---- driver ------------------------------------------------------------------
OUTDIR="-"
[ "$1" = "-o" ] && { OUTDIR="$2"; shift 2; mkdir -p "$OUTDIR"; }
BENCHES=("$@")
[ ${#BENCHES[@]} -eq 0 ] && BENCHES=(401.bzip2 429.mcf 444.namd 458.sjeng 445.gobmk 464.h264ref 456.hmmer 482.sphinx3 470.lbm 473.astar)
JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}
RESULTS=$(mktemp -d); trap 'rm -rf "$RESULTS"' EXIT

for B in "${BENCHES[@]}"; do
  SRC="$SPEC/$B/src"; [ -d "$SRC" ] || continue
  find "$SRC" \( -name '*.c' -o -name '*.cpp' -o -name '*.cc' -o -name '*.C' \) -print0 \
    | xargs -0 -P "$JOBS" -I {} bash "$0" --worker "$B" "$RESULTS" "$OUTDIR" {}
done

echo "bench,func,base,best,policy,final"
cat "$RESULTS"/tu.* 2>/dev/null

{
  echo "================ autotuner suite summary ================"
  echo "trial policies: none ${POLS[*]}   (JOBS=$JOBS)"
  cat "$RESULTS"/tu.* 2>/dev/null | awk -F, '
    { tb+=$3; to+=$4; tf+=$6; if ($6!=$4) mm++; cnt[$5]++ }
    END {
      printf "total base text:   %d\n", tb
      printf "total final text:  %d   (delta %+d)\n", tf, tf-tb
      printf "predicted oracle:  %d   (delta %+d)\n", to, to-tb
      printf "verify mismatches (final != predicted): %d\n", mm
      printf "policy picks:"; for (p in cnt) printf " %s=%d", p, cnt[p]; print ""
    }'
} >&2
