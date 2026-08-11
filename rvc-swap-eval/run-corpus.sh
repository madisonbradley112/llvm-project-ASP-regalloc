#!/bin/bash
# Corpus A/B: compile musl's C sources for rv64gc at -Os with the pass off and
# on, and report the .text delta. Object files are enough -- relocations affect
# both arms identically, so the delta is valid even unlinked.
#
# Needs musl sources. Easiest source of them:  pip install ziglang
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$(cd "$HERE/../build" && pwd)"
CLANG="$BUILD/bin/clang"
OBJDUMP="$BUILD/bin/llvm-objdump"

Z=$(python3 -c "import ziglang,os;print(os.path.dirname(ziglang.__file__))" 2>/dev/null) || {
  echo "musl sources not found. Run: pip install ziglang" >&2; exit 1; }
M="$Z/lib/libc/musl"
INC="-I$M/src/include -I$M/src/internal -I$M/include \
     -I$Z/lib/libc/include/riscv64-linux-musl -I$Z/lib/libc/include/generic-musl \
     -I$M/arch/riscv64 -I$M/arch/generic"
FLAGS="--target=riscv64-unknown-linux-musl -march=rv64gc -Os -c -w -ffreestanding"

textsize() { $OBJDUMP -h "$1" 2>/dev/null | awk '$2==".text"{print strtonum("0x"$3)}'; }

for MODE in off on; do
  OUT="$HERE/obj-$MODE"; rm -rf "$OUT"; mkdir -p "$OUT"
  EXTRA=""; [ "$MODE" = off ] && EXTRA="-mllvm -riscv-swap-enable=false"
  n=0
  for f in $(find "$M/src" -name '*.c' | sort); do
    o="$OUT/$(echo "$f" | md5sum | cut -c1-12).o"
    "$CLANG" $FLAGS $INC $EXTRA "$f" -o "$o" 2>/dev/null && n=$((n+1))
  done
  tot=0
  for o in "$OUT"/*.o; do s=$(textsize "$o"); tot=$((tot + ${s:-0})); done
  echo "$MODE: $n objects, .text total = $tot bytes"
  echo "$tot" > "$HERE/total-$MODE.txt"
done

A=$(cat "$HERE/total-off.txt"); B=$(cat "$HERE/total-on.txt")
python3 -c "a=$A;b=$B;print(f'\ndelta = {b-a:+d} bytes  ({(b-a)/a*100:+.3f}%)')"

echo
echo "=== aggregate stats with pass on ==="
"$CLANG" $FLAGS $INC "$M/src/stdio/vfprintf.c" -o /dev/null -mllvm -stats 2>&1 \
  | grep -i "register-swap" || true
