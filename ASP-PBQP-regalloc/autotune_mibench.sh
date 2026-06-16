#!/bin/bash
# Per-function compression autotuner for the MiBench suite (Makefile-driven).
#
# "All benchmarks" for MiBench does NOT mean every .c on disk -- hundreds are
# alternative-platform backends (jmemdos.c, windows/X11/audio variants) the
# build never compiles together.  Instead we ask each benchmark's own build
# system, via `make -Bn` (forced dry-run), for the exact source list it compiles
# and the -D defines it needs (e.g. lout/typeset's -DOS_UNIX=1, gsm's -DSASR).
# Benchmarks whose Makefile is autoconf-generated and won't configure on this
# host (tiff/mad/sphinx/rsynth/ispell) fall back to all-.c with skip-on-fail, so
# alien-platform / POSIX-only files drop out and the rest still count.  Target
# stays bare-metal rv32e (consistent with the SPEC/PolyBench numbers).
#
# Env: POLICIES="all skip skip2 skip3"  JOBS=<n>
#   bash ASP-PBQP-regalloc/autotune_mibench.sh [-o OUTDIR]

ROOT=/Users/jupiterbradley/Documents/University/msc-thesis/clingo-regalloc/llvm-project-ASP-regalloc
CLANG=$ROOT/build/bin/clang
NM=$ROOT/build/bin/llvm-nm
INC=/opt/homebrew/Cellar/riscv-gnu-toolchain/main/riscv64-unknown-elf/include
MI=$ROOT/mibench
COMMONC="--target=riscv32-unknown-elf -mabi=${MABI:-ilp32e} -isystem $INC -march=${MARCH:-rv32e_zca_zcb_zcmp} -Os -w -ffunction-sections -fdata-sections -Wno-implicit-function-declaration -Wno-implicit-int -Wno-return-type -Wno-int-conversion"

read -r -a POLS <<< "${POLICIES:-all skip skip2 skip3}"

polflags(){
  case "$1" in
    all)   echo "-mllvm -riscv-asp-split -mllvm -riscv-asp-split-naive-gprc";;
    skip)  echo "-mllvm -riscv-asp-split -mllvm -riscv-asp-split-naive-gprc -mllvm -riscv-asp-split-skip-xcall";;
    skip*) echo "-mllvm -riscv-asp-split -mllvm -riscv-asp-split-naive-gprc -mllvm -riscv-asp-split-skip-xcall -mllvm -riscv-asp-split-xcall-threshold=${1#skip}";;
  esac
}
syms(){ "$NM" --print-size --defined-only --radix=d "$1" 2>/dev/null | awk '$3=="t"||$3=="T"{print $4, $2+0}' | sort -k1,1; }

# ---- worker: process one .c file ---------------------------------------------
# argv: --worker <benchname> <benchdir> <defs> <resultsdir> <outdir|-> <file>
if [ "$1" = "--worker" ]; then
  B="$2"; BDIR="$3"; DEFS="$4"; RESULTS="$5"; OUTDIR="$6"; f="$7"
  INCDIRS=(); while IFS= read -r d; do INCDIRS+=(-I "$d"); done < <(find "$BDIR" -type d)
  CC=($CLANG $COMMONC $DEFS "${INCDIRS[@]}")
  W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
  Bsan=$(echo "$B" | tr /. __)
  "${CC[@]}" -c "$f" -o "$W/none.o" 2>/dev/null || { : > "$RESULTS/fail.$Bsan.$$"; exit 0; }
  : > "$W/sizes.txt"
  syms "$W/none.o" | awk '{print $1, "none", $2}' >> "$W/sizes.txt"
  for P in "${POLS[@]}"; do
    "${CC[@]}" $(polflags "$P") -c "$f" -o "$W/p.o" 2>/dev/null || { : > "$RESULTS/fail.$Bsan.$$"; exit 0; }
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
     -c "$f" -o "$W/final.o" 2>/dev/null || { : > "$RESULTS/fail.$Bsan.$$"; exit 0; }
  [ "$OUTDIR" != "-" ] && cp "$W/final.o" "$OUTDIR/$Bsan-$(basename "$f").o"
  syms "$W/final.o" > "$W/fin.txt"
  : > "$RESULTS/ok.$Bsan.$$"
  out=$(mktemp "$RESULTS/tu.XXXXXX")
  join "$W/decided.txt" "$W/fin.txt" 2>/dev/null | awk -v bench="$B" '
    { printf "%s,%s,%d,%d,%s,%d\n", bench, $1, $2, $3, $4, $5 }' > "$out"
  exit 0
fi

# ---- per-benchmark build spec: source list + defines via `make -Bn` ----------
# Echoes defines on line 1 (prefixed "DEFS ") then one resolved source path per
# line.  Falls back to all .c in the dir when make yields nothing.
buildspec(){
  local BDIR="$1" B="$2" MK out target
  MK=$(find "$BDIR" -maxdepth 2 -iname 'Makefile' | head -1)
  target=""; case "$B" in security/pgp) target="linux";; esac
  out=""
  [ -n "$MK" ] && out=$( cd "$(dirname "$MK")" && make -Bn $target 2>/dev/null )
  local defs srcs
  defs=$(printf '%s\n' "$out" | grep -oE '\-D[A-Za-z_][A-Za-z0-9_]*(=[^ ]*)?' | sort -u | tr '\n' ' ')
  srcs=$(printf '%s\n' "$out" | grep -oE '[A-Za-z0-9_./-]+\.c\b' | sort -u)
  echo "DEFS $defs"
  if [ -n "$srcs" ]; then
    # resolve each basename to a real path inside the benchmark dir
    printf '%s\n' "$srcs" | while IFS= read -r s; do
      [ -f "$s" ] && { echo "$s"; continue; }
      find "$BDIR" -name "$(basename "$s")" | head -1
    done | sort -u | grep -v '^$'
  else
    find "$BDIR" -name '*.c' | sort -u   # fallback: all .c, skip-on-fail
  fi
}

# ---- driver ------------------------------------------------------------------
OUTDIR="-"
[ "$1" = "-o" ] && { OUTDIR="$2"; shift 2; mkdir -p "$OUTDIR"; }
JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}
RESULTS=$(mktemp -d); trap 'rm -rf "$RESULTS"' EXIT

echo "bench,func,base,best,policy,final" > "$RESULTS/header"
for BDIR in $(find "$MI" -mindepth 2 -maxdepth 2 -type d ! -path '*automotive 2*' ! -name 'tiff-data' | sort); do
  B=$(echo "$BDIR" | sed "s#$MI/##")
  spec=$(buildspec "$BDIR" "$B")
  DEFS=$(printf '%s\n' "$spec" | sed -n '1s/^DEFS //p')
  printf '%s\n' "$spec" | tail -n +2 | grep -v '^$' | tr '\n' '\0' \
    | xargs -0 -P "$JOBS" -I {} bash "$0" --worker "$B" "$BDIR" "$DEFS" "$RESULTS" "$OUTDIR" {}
done

cat "$RESULTS/header"; cat "$RESULTS"/tu.* 2>/dev/null

{
  echo "================ MiBench autotuner suite summary ================"
  echo "trial policies: none ${POLS[*]}   (JOBS=$JOBS)"
  cat "$RESULTS"/tu.* 2>/dev/null | awk -F, '
    { tb+=$3; to+=$4; tf+=$6; if ($6!=$4) mm++; cnt[$5]++ }
    END {
      printf "rows (funcs): %d\n", NR
      printf "total base text:   %d\n", tb
      printf "total final text:  %d   (delta %+d, %.3f%%)\n", tf, tf-tb, (tf?(tf-tb)/tb*100:0)
      printf "verify mismatches: %d\n", mm
      printf "policy picks:"; for (p in cnt) printf " %s=%d", p, cnt[p]; print ""
    }'
  echo "--- compile coverage (TUs compiled / listed) ---"
  for ext in ok fail; do for g in "$RESULTS"/$ext.*; do [ -e "$g" ] || continue
    bn=$(basename "$g"); bn=${bn#$ext.}; bn=${bn%.*}; echo "$ext $bn"; done; done \
    | awk '{c[$2"_"$1]++; seen[$2]=1} END{for(b in seen) printf "%-26s %d/%d\n", b, c[b"_ok"]+0, c[b"_ok"]+c[b"_fail"]}' | sort
} >&2
