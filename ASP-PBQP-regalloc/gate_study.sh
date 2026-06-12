#!/bin/bash
# Gate study: naive GPRC class-narrowing (no solver), thrash-gated, vs greedy.
#
# Runs the full SPEC suite once per gate setting via run_suite.sh (which
# compiles every file twice -- plain greedy vs the pass -- and reports dText).
# The naive mode runs NO solver, so each full-suite pass is ordinary-compile
# fast.  Each setting writes its own results file and is RESUMABLE: relaunch
# the same command and completed benchmarks are skipped.
#
#   nohup bash ASP-PBQP-regalloc/gate_study.sh > ~/gate_study.log 2>&1 &
#   tail -f ~/gate_study.log
#   bash ASP-PBQP-regalloc/gate_study.sh summary     # matrix from whatever's done
#
# Env knobs:
#   GATES="0 14 14:50 14:80"  settings to sweep; "lo" = lower-bound gate only,
#                             "lo:hi" = band gate (skip functions whose max
#                             live-at-entry is outside [lo,hi]); 0 = ungated.
#   OUTDIR=$HOME/gate_study   results directory
#   plus everything run_suite.sh accepts (MARCH/TARGET/MABI/OPT/benchmark args).

ROOT=/Users/jupiterbradley/Documents/University/msc-thesis/clingo-regalloc/llvm-project-ASP-regalloc
SUITE="$ROOT/ASP-PBQP-regalloc/run_suite.sh"
OUTDIR=${OUTDIR:-$HOME/gate_study}
GATES=${GATES:-"0 14 14:50 14:80 20:80"}
mkdir -p "$OUTDIR"

summary() {
  # Combine per-gate results files into a benchmark x gate dText matrix.
  python3 - "$OUTDIR" <<'PY'
import sys, os, re, glob
outdir = sys.argv[1]
data, gates, benches = {}, [], []
def tag_of(p): return re.search(r"gate_([\d-]+)\.txt", p).group(1)
def key_of(t): return [int(x) for x in t.split("-")]
for f in sorted(glob.glob(os.path.join(outdir, "gate_*.txt")),
                key=lambda p: key_of(tag_of(p))):
    g = tag_of(f)
    gates.append(g)
    for line in open(f):
        m = re.match(r"(\S+)\s+\S+\s+\|\s+(\d+)\s+(\d+)\s+\|\s+([+-]\d+)", line)
        if m:
            b, base, d = m.group(1), int(m.group(2)), int(m.group(4))
            data[(b, g)] = (d, base)
            if b not in benches: benches.append(b)
if not gates:
    print("no gate_*.txt results yet in", outdir); sys.exit(0)
hdr = "bench".ljust(16) + "base_text".rjust(10) + "".join((" g=%s" % g).rjust(10) for g in gates)
print(hdr); print("-" * len(hdr))
tot = {g: 0 for g in gates}; totbase = 0; complete = True
for b in benches:
    base = next((data[(b, g)][1] for g in gates if (b, g) in data), 0)
    row = b.ljust(16) + str(base).rjust(10)
    for g in gates:
        if (b, g) in data:
            d = data[(b, g)][0]; tot[g] += d
            row += ("%+d" % d).rjust(10)
        else:
            row += "..".rjust(10); complete = False
    totbase += base
    print(row)
print("-" * len(hdr))
row = "NET".ljust(16) + str(totbase).rjust(10)
for g in gates:
    row += ("%+d" % tot[g]).rjust(10)
print(row)
row = "NET %".ljust(16) + "".rjust(10)
for g in gates:
    row += ("%.3f%%" % (100.0 * tot[g] / totbase if totbase else 0)).rjust(10)
print(row)
if not complete:
    print("(.. = benchmark not finished for that gate yet)")
PY
}

if [ "$1" = "summary" ]; then summary; exit 0; fi

for K in $GATES; do
  LO="${K%%:*}"; HI=""
  case "$K" in *:*) HI="${K##*:}";; esac
  TAG="$LO"; [ -n "$HI" ] && TAG="${LO}-${HI}"
  FLAGS="-mllvm -riscv-asp-split -mllvm -riscv-asp-split-naive-gprc -mllvm -riscv-asp-split-naive-gate=$LO"
  [ -n "$HI" ] && FLAGS="$FLAGS -mllvm -riscv-asp-split-naive-gate-max=$HI"
  echo "===== gate=$K (out: gate_$TAG.txt) ====="
  OUT="$OUTDIR/gate_$TAG.txt" ASPFLAGS="$FLAGS" bash "$SUITE" "$@"
done
echo "===== all gates done; summary ====="
summary
