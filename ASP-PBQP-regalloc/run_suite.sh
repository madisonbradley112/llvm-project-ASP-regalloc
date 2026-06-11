#!/bin/bash
# Standalone, resumable SPEC code-size harness for the ASP register-allocation
# work.  Designed to be launched in your own terminal with nohup so it survives
# for hours, writing results incrementally to a file you (or Claude) inspect later.
#
#   nohup bash ASP-PBQP-regalloc/run_suite.sh > ~/asp_run.log 2>&1 &
#   tail -f ~/asp_run.log              # watch progress
#   cat  ~/asp_suite_results.txt       # the result table (grows as it goes)
#
# Defaults run the binary-search splitting pass on RV32E. Override via env:
#   MARCH=rv64g_zca_zcb_zcmp TARGET=riscv64-unknown-elf MABI= \
#   ASPFLAGS="-mllvm -riscv-asp-rvc-regalloc" OUT=~/phase1.txt \
#   bash ASP-PBQP-regalloc/run_suite.sh 470.lbm 429.mcf
#
# It is RESUMABLE: a benchmark already present in $OUT is skipped, so if the run
# is interrupted just relaunch the same command and it continues.

set +u

ROOT=/Users/jupiterbradley/Documents/University/msc-thesis/clingo-regalloc/llvm-project-ASP-regalloc
CLANG=${CLANG:-$ROOT/build/bin/clang}
OD=${OD:-$ROOT/build/bin/llvm-objdump}
INC=${INC:-/opt/homebrew/Cellar/riscv-gnu-toolchain/main/riscv64-unknown-elf/include}
SPEC=${SPEC:-"/Users/jupiterbradley/Virtual Machines.localized/ubuntu-shared-folder/reimplementing-compression-aware-register-allocator/app/cpu2006/benchspec/CPU2006"}

TARGET=${TARGET:-riscv32-unknown-elf}
MARCH=${MARCH:-rv32e_zca_zcb_zcmp}
MABI=${MABI:--mabi=ilp32e}            # set MABI= (empty) for rv64
OPT=${OPT:--Os}
ASPFLAGS=${ASPFLAGS:-"-mllvm -riscv-asp-split -mllvm -riscv-asp-split-time-limit=1 -mllvm -riscv-asp-split-max-points=300"}
OUT=${OUT:-$HOME/asp_suite_results.txt}

# Default benchmark list (small -> large) if none given on the command line.
BENCHES=("$@")
if [ ${#BENCHES[@]} -eq 0 ]; then
  BENCHES=(462.libquantum 470.lbm 429.mcf 473.astar 444.namd 401.bzip2 \
           458.sjeng 433.milc 482.sphinx3 456.hmmer 445.gobmk 464.h264ref)
fi

CXXINC=(-isystem "$INC/c++/13.2.0" -isystem "$INC/c++/13.2.0/riscv64-unknown-elf")
COMMON=(--target=$TARGET $MABI -isystem "$INC" -march=$MARCH $OPT -w -DSPEC_CPU -DNDEBUG)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

log(){ echo "[$(date '+%H:%M:%S')] $*"; }

# count <obj> -> "<#compressed> <#total_text_bytes>"
count(){ "$OD" -d "$1" 2>/dev/null | awk '/^[ ]+[0-9a-f]+:/{h=$2; if(length(h)==4){c++;b+=2}else if(length(h)==8){b+=4}} END{printf "%d %d",c+0,b+0}'; }

# Header (only if the results file is new).
if [ ! -f "$OUT" ]; then
  printf "%-16s %-8s | %8s %8s | %7s %s\n" bench march text_b text_a dText files > "$OUT"
fi

log "START march=$MARCH asp='$ASPFLAGS' out=$OUT"
for B in "${BENCHES[@]}"; do
  if grep -q "^$B " "$OUT" 2>/dev/null; then log "skip $B (already in $OUT)"; continue; fi
  SRC="$SPEC/$B/src"
  if [ ! -d "$SRC" ]; then log "$B: no src dir"; continue; fi
  log "begin $B"
  INCDIRS=(); while IFS= read -r d; do INCDIRS+=(-I "$d"); done < <(find "$SRC" -type d)
  tb=0; ta=0; nok=0; ntot=0
  while IFS= read -r f; do
    ntot=$((ntot+1)); lang=()
    case "${f##*.}" in cpp|cc|C) lang=("${CXXINC[@]}");; esac
    ob="$WORK/b.o"; oa="$WORK/a.o"; rm -f "$ob" "$oa"
    "$CLANG" "${COMMON[@]}" "${lang[@]}" "${INCDIRS[@]}" -c "$f" -o "$ob" 2>/dev/null
    "$CLANG" "${COMMON[@]}" $ASPFLAGS "${lang[@]}" "${INCDIRS[@]}" -c "$f" -o "$oa" 2>/dev/null
    [ -f "$ob" ] && [ -f "$oa" ] || { log "  skip file $(basename "$f") (compile failed)"; continue; }
    nok=$((nok+1))
    read cb bb <<<"$(count "$ob")"; tb=$((tb+bb))
    read ca bbn <<<"$(count "$oa")"; ta=$((ta+bbn))
    log "  $(basename "$f"): base=$bb asp=$bbn  (running benchmark total dText=$((ta-tb)))"
  done < <(find "$SRC" \( -name '*.c' -o -name '*.cpp' -o -name '*.cc' -o -name '*.C' \))
  printf "%-16s %-8s | %8d %8d | %+7d %d/%d\n" "$B" "$MARCH" "$tb" "$ta" "$((ta-tb))" "$nok" "$ntot" >> "$OUT"
  log "done  $B  dText=$((ta-tb))  files=$nok/$ntot"
done
log "ALL DONE"
echo "# ALL_DONE" >> "$OUT"
