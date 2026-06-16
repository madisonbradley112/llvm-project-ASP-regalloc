#!/bin/bash
# Tier-1 checkpoint autotuner for SPEC: same best-of-N policy search as
# autotune.sh, but the front-end + IR-optimization pipeline runs ONCE per TU.
#
# Insight: the policies differ only at register-allocation time and later, yet
# the external autotuner re-runs the whole compile (front-end, IR opt, ISel,
# RA, emit) N+1 times. The front-end dominates for header-heavy C/C++, so we
# checkpoint at optimized LLVM IR (`clang -emit-llvm -Os`, run once) and resume
# each policy with `llc -O2`. This is BYTE-FAITHFUL to a direct `clang -Os -c`
# (verified: llc -O2 on the .bc reproduces the exact per-function text sizes),
# because the optsize/minsize attributes carried in the IR drive llc's codegen.
# (A finer MIR checkpoint that also shares ISel was tried and is NOT faithful --
# clang's stop-after MIR + llc resume diverges -- so we checkpoint at IR.)
#
# Env: POLICIES="all skip skip2 skip3"  JOBS=<n>  MARCH=  MABI=
#   bash ASP-PBQP-regalloc/autotune_ckpt.sh [-o OUTDIR] <benches...>

ROOT=/Users/jupiterbradley/Documents/University/msc-thesis/clingo-regalloc/llvm-project-ASP-regalloc
CLANG=$ROOT/build/bin/clang
LLC=$ROOT/build/bin/llc
NM=$ROOT/build/bin/llvm-nm
INC=/opt/homebrew/Cellar/riscv-gnu-toolchain/main/riscv64-unknown-elf/include
SPEC="/Users/jupiterbradley/Virtual Machines.localized/ubuntu-shared-folder/reimplementing-compression-aware-register-allocator/app/cpu2006/benchspec/CPU2006"
CXXINC=(-isystem "$INC/c++/13.2.0" -isystem "$INC/c++/13.2.0/riscv64-unknown-elf" -std=gnu++98)
COMMONC="--target=riscv32-unknown-elf -mabi=${MABI:-ilp32e} -isystem $INC -march=${MARCH:-rv32e_zca_zcb_zcmp} -Os -w -ffunction-sections -fdata-sections -DSPEC_CPU -DSPEC_CPU_LINUX -DNDEBUG -Wno-implicit-function-declaration -Wno-implicit-int -Wno-return-type -Wno-int-conversion"
LLCFLAGS="-O2 -function-sections"   # -O2 reproduces clang -Os codegen via the IR's optsize attrs

read -r -a POLS <<< "${POLICIES:-all skip skip2 skip3}"

# Policy token -> llc flags (same options as the pass, no -mllvm prefix for llc).
polflags(){
  case "$1" in
    all)   echo "-riscv-asp-split -riscv-asp-split-naive-gprc";;
    skip)  echo "-riscv-asp-split -riscv-asp-split-naive-gprc -riscv-asp-split-skip-xcall";;
    skip*) echo "-riscv-asp-split -riscv-asp-split-naive-gprc -riscv-asp-split-skip-xcall -riscv-asp-split-xcall-threshold=${1#skip}";;
  esac
}
syms(){ "$NM" --print-size --defined-only --radix=d "$1" 2>/dev/null | awk '$3=="t"||$3=="T"{print $4, $2+0}' | sort -k1,1; }

# ---- worker: one TU --- argv: --worker <bench> <resultsdir> <outdir|-> <file>
if [ "$1" = "--worker" ]; then
  B="$2"; RESULTS="$3"; OUTDIR="$4"; f="$5"
  SRC="$SPEC/$B/src"
  EXTRA=(); [ "$B" = "433.milc" ] && EXTRA=(-DFN)
  INCDIRS=(); while IFS= read -r d; do INCDIRS+=(-I "$d"); done < <(find "$SRC" -type d)
  lang=(); case "${f##*.}" in cpp|cc|C) lang=("${CXXINC[@]}");; esac
  W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
  # 1) front-end + IR-opt ONCE -> optimized bitcode
  $CLANG $COMMONC "${EXTRA[@]}" "${lang[@]}" "${INCDIRS[@]}" -emit-llvm -c "$f" -o "$W/o.bc" 2>/dev/null || exit 0
  # 2) baseline + each policy: resume from IR with llc
  : > "$W/sizes.txt"
  "$LLC" $LLCFLAGS "$W/o.bc" --filetype=obj -o "$W/none.o" 2>/dev/null || exit 0
  syms "$W/none.o" | awk '{print $1, "none", $2}' >> "$W/sizes.txt"
  for P in "${POLS[@]}"; do
    "$LLC" $LLCFLAGS $(polflags "$P") "$W/o.bc" --filetype=obj -o "$W/p.o" 2>/dev/null || exit 0
    syms "$W/p.o" | awk -v t="$P" '{print $1, t, $2}' >> "$W/sizes.txt"
  done
  # 3) pick smallest policy per function (ties favour none); write policy map
  : > "$W/policy.txt"
  awk '{ name=$1; tag=$2; sz=$3;
         if (tag=="none") base[name]=sz;
         if (!(name in bestsz) || sz < bestsz[name]) { bestsz[name]=sz; bestpol[name]=tag } }
       END { for (n in bestsz) { if (bestpol[n]!="none") print n, bestpol[n] > "'"$W/policy.txt"'";
                                 print n, base[n], bestsz[n], bestpol[n] } }' \
       "$W/sizes.txt" | sort -k1,1 > "$W/decided.txt"
  # 4) final resume honouring the per-function policy map
  "$LLC" $LLCFLAGS -riscv-asp-split -riscv-asp-split-policy-file="$W/policy.txt" \
     "$W/o.bc" --filetype=obj -o "$W/final.o" 2>/dev/null || exit 0
  [ "$OUTDIR" != "-" ] && cp "$W/final.o" "$OUTDIR/$(echo "$B-$(basename "$f")" | tr / _).o"
  syms "$W/final.o" > "$W/fin.txt"
  out=$(mktemp "$RESULTS/tu.XXXXXX")
  join "$W/decided.txt" "$W/fin.txt" 2>/dev/null | awk -v bench="$B" \
    '{ printf "%s,%s,%d,%d,%s,%d\n", bench, $1, $2, $3, $4, $5 }' > "$out"
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
  echo "============ Tier-1 checkpoint autotuner summary ============"
  echo "trial policies: none ${POLS[*]}   (JOBS=$JOBS, MARCH=${MARCH:-rv32e_zca_zcb_zcmp}, MABI=${MABI:-ilp32e})"
  cat "$RESULTS"/tu.* 2>/dev/null | awk -F, '
    { tb+=$3; to+=$4; tf+=$6; if ($6!=$4) mm++; cnt[$5]++ }
    END { printf "rows (funcs): %d\n", NR
          printf "total base text:   %d\n", tb
          printf "total final text:  %d   (delta %+d, %.3f%%)\n", tf, tf-tb, (tb?(tf-tb)/tb*100:0)
          printf "verify mismatches: %d\n", mm
          printf "policy picks:"; for (p in cnt) printf " %s=%d", p, cnt[p]; print "" }'
} >&2
