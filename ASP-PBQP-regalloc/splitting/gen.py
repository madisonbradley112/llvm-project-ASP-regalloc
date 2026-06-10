#!/usr/bin/env python3
# Realistic straight-line instance generator for the per-point splitting model.
# Models a sequence of instructions (one per point): each instruction defines a
# fresh value and uses 1-2 values defined earlier that are still within a reuse
# window.  A value's live range spans from its def to its last use.  This keeps
# uses-per-point <= 2 (like real instructions) so pressure comes from LIVENESS
# (values held across spans) -- exactly the regime where splitting matters --
# rather than from unsatisfiable simultaneous-use.
#
# Usage: gen.py NINSN NREGS [REUSE_WINDOW] [USE_PROB] [SEED]
#   NINSN          number of instructions = points = values
#   NREGS          physical registers (lower half gprc)
#   REUSE_WINDOW   how far back a use may reach (controls pressure; default 16)
#   USE_PROB       prob. of a 2nd operand (default 0.6)
#   SEED           RNG seed (default 0)
import sys, random

n      = int(sys.argv[1])
nr     = int(sys.argv[2])
window = int(sys.argv[3]) if len(sys.argv) > 3 else 16
usep   = float(sys.argv[4]) if len(sys.argv) > 4 else 0.6
seed   = int(sys.argv[5]) if len(sys.argv) > 5 else 0
random.seed(seed)

last_use = list(range(n))          # last_use[v]; starts at def point
uses = {}                           # point -> list of values used there
for p in range(n):
    ops = []
    cand_lo = max(0, p - window)
    pool = list(range(cand_lo, p))
    if pool:
        ops.append(random.choice(pool))
        if random.random() < usep and len(pool) > 1:
            o2 = random.choice(pool)
            if o2 != ops[0]:
                ops.append(o2)
    uses[p] = ops
    for v in ops:
        last_use[v] = max(last_use[v], p)

# Live range of value v: [v (def), last_use[v]].
cnt = [0] * (n + 1)
for v in range(n):
    for p in range(v, last_use[v] + 1):
        cnt[p] += 1
peak = max(cnt) if cnt else 0
maxuse = max((len(u) for u in uses.values()), default=0)
print(f"% NINSN={n} NREGS={nr} window={window} ACTUAL_PEAK={peak} max_uses_per_point={maxuse}",
      file=sys.stderr)

out = [f"point(0..{n-1}).",
       "reg(0..%d)." % (nr - 1),
       "gprc(0..%d)." % (nr // 2 - 1),
       "storecost(4). reloadcost(4). compsave(2)."]
for v in range(n):
    out.append(f"live({v},{v}..{last_use[v]}). def({v},{v}).")
for p in range(n):
    for v in uses[p]:
        out.append(f"use({v},{p}).")
        # a use is a compression candidate ~half the time
        if (p + v) % 2 == 0:
            out.append(f"cand(c{p}_{v},{v},{p}).")
print("\n".join(out))
