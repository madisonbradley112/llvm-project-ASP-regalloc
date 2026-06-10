#!/usr/bin/env python3
# Straight-line instance with a LOW-HIGH-LOW pressure profile: the reuse window
# (how far back a use reaches -> how long values stay live -> pressure) ramps
# triangularly from WLOW at the ends to WHIGH in the middle.  Used to exhibit
# re-widening in the decomposers: an easy prologue and epilogue (wide windows)
# bracketing a dense middle (narrow windows).
#
# Usage: gen_profile.py NINSN NREGS [WLOW] [WHIGH] [SEED]
import sys, random
n     = int(sys.argv[1])
nr    = int(sys.argv[2])
wlow  = int(sys.argv[3]) if len(sys.argv) > 3 else 3
whigh = int(sys.argv[4]) if len(sys.argv) > 4 else 22
seed  = int(sys.argv[5]) if len(sys.argv) > 5 else 0
random.seed(seed)

def window_at(p):
    t = 1.0 - abs(2.0*p/(n-1) - 1.0)        # 0 at ends, 1 at middle
    return max(1, int(round(wlow + (whigh - wlow) * t)))

last_use = list(range(n)); uses = {}
for p in range(n):
    w = window_at(p)
    pool = list(range(max(0, p-w), p))
    ops = []
    if pool:
        ops.append(random.choice(pool))
        if random.random() < 0.6 and len(pool) > 1:
            o2 = random.choice(pool)
            if o2 != ops[0]: ops.append(o2)
    uses[p] = ops
    for v in ops: last_use[v] = max(last_use[v], p)

cnt = [0]*(n+1)
for v in range(n):
    for p in range(v, last_use[v]+1): cnt[p] += 1
print(f"% profile low-high-low NINSN={n} NREGS={nr} peak={max(cnt)} "
      f"win[{wlow}..{whigh}]", file=sys.stderr)

out = [f"point(0..{n-1}).", f"reg(0..{nr-1}).", f"gprc(0..{nr//2-1}).",
       "storecost(4). reloadcost(4). compsave(2)."]
for v in range(n):
    out.append(f"live({v},{v}..{last_use[v]}). def({v},{v}).")
for p in range(n):
    for v in uses[p]:
        out.append(f"use({v},{p}).")
        if (p+v) % 2 == 0: out.append(f"cand(c{p}_{v},{v},{p}).")
print("\n".join(out))
