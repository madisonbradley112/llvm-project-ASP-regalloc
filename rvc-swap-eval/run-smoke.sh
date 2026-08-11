#!/bin/bash
# Smoke test: does the pass fire at all, and how often?
set -e
BUILD="$(cd "$(dirname "$0")/../build" && pwd)"
CLANG="$BUILD/bin/clang"
SRC="$(dirname "$0")/smoke.c"
FLAGS="--target=riscv64-unknown-elf -march=rv64gc -Os -c -ffreestanding -nostdlib"

echo "=== stats (pass ON) ==="
"$CLANG" $FLAGS "$SRC" -o /tmp/smoke_on.o -mllvm -stats 2>&1 \
  | grep -Ei "riscv-make-compressible-register-swap|Candidates|split|compressible|ceiling|sunk|hoisted" || echo "(no stats lines -- pass did not fire)"

echo
echo "=== debug trace ==="
"$CLANG" $FLAGS "$SRC" -o /tmp/smoke_dbg.o \
  -mllvm -debug-only=riscv-make-compressible-register-swap 2>&1 | head -40 || true

echo
echo "=== size A/B ==="
"$CLANG" $FLAGS "$SRC" -o /tmp/smoke_off.o -mllvm -riscv-swap-enable=false
for f in /tmp/smoke_off.o /tmp/smoke_on.o; do
  printf "%-20s .text = %s bytes\n" "$(basename $f)" \
    "$($BUILD/bin/llvm-objdump -h $f | awk '$2==".text"{print strtonum("0x"$3)}')"
done
