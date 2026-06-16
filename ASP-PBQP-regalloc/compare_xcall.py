#!/usr/bin/env python3
# Compare per-candidate narrowing (skip-xcall) against constrain-all and the
# function-level call gate.
#   python3 compare_xcall.py ~/profit2.csv ~/xcall.csv
import sys, csv

def load(path, dcol):
    d = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            try:
                d[(r["bench"], r["func"])] = (int(r["base_bytes"]), int(r[dcol]))
            except (KeyError, ValueError):
                pass
    return d

prof = load(sys.argv[1], "delta_bytes")     # constrain-all (+ features)
xc   = load(sys.argv[2], "delta_skipxcall")  # per-candidate

# also load the features from profit2 so we can apply the call gate
feat = {}
with open(sys.argv[1]) as f:
    for r in csv.DictReader(f):
        try:
            feat[(r["bench"], r["func"])] = {
                "cands": int(r["cands"]), "calls": int(r["calls"]),
                "ncand": int(r.get("ncand", 0)), "xcall": int(r.get("xcall", 0)),
            }
        except (KeyError, ValueError):
            pass

keys = sorted(set(prof) & set(xc))
print(f"functions present in both: {len(keys)}")

ca   = sum(prof[k][1] for k in keys)                       # constrain-all everywhere
sx   = sum(xc[k][1]   for k in keys)                       # skip-xcall everywhere
orac = sum(prof[k][1] for k in keys if prof[k][1] < 0)     # oracle (constrain-all)
# function-level call gate: calls/cands <= 0.09, else don't narrow (delta 0)
def gate(k):
    fe = feat.get(k);
    if not fe or fe["cands"] == 0: return False
    return fe["calls"]/fe["cands"] <= 0.09
gca  = sum(prof[k][1] for k in keys if gate(k))            # gated constrain-all
gsx  = sum(xc[k][1]   for k in keys if gate(k))            # gated skip-xcall
# best of both per function (lower bound: pick the better policy per func)
both = sum(min(prof[k][1], xc[k][1]) for k in keys)

def winlose(vals):
    return sum(1 for v in vals if v<0), sum(1 for v in vals if v>0)

print(f"  constrain-all (everywhere):      net {ca:+7d}   "
      f"win/lose {winlose([prof[k][1] for k in keys])}")
print(f"  skip-xcall   (everywhere):       net {sx:+7d}   "
      f"win/lose {winlose([xc[k][1] for k in keys])}")
print(f"  constrain-all + call gate(.09):  net {gca:+7d}")
print(f"  skip-xcall    + call gate(.09):  net {gsx:+7d}")
print(f"  per-func best-of-both (LB):      net {both:+7d}")
print(f"  oracle ceiling (constrain-all):  net {orac:+7d}")
print()

# Where does skip-xcall beat / lose vs constrain-all?
better = [(k, xc[k][1]-prof[k][1]) for k in keys if xc[k][1] < prof[k][1]]
worse  = [(k, xc[k][1]-prof[k][1]) for k in keys if xc[k][1] > prof[k][1]]
print(f"skip-xcall better than constrain-all on {len(better)} funcs "
      f"(sum improvement {sum(d for _,d in better):+d})")
print(f"skip-xcall worse  than constrain-all on {len(worse)} funcs "
      f"(sum regression  {sum(d for _,d in worse):+d})")
print()
print("top 10 funcs where skip-xcall HELPS vs constrain-all (avoids a regression):")
for (k,d) in sorted(better, key=lambda t:t[1])[:10]:
    print(f"  {d:+6d}  {k[0]:<12}{k[1]:<28} constrain-all={prof[k][1]:+d} skip-xcall={xc[k][1]:+d}"
          + (f" xcall={feat[k]['xcall']}/{feat[k]['ncand']}" if k in feat else ""))
print("top 10 funcs where skip-xcall HURTS vs constrain-all (drops a real win):")
for (k,d) in sorted(worse, key=lambda t:-t[1])[:10]:
    print(f"  {d:+6d}  {k[0]:<12}{k[1]:<28} constrain-all={prof[k][1]:+d} skip-xcall={xc[k][1]:+d}"
          + (f" xcall={feat[k]['xcall']}/{feat[k]['ncand']}" if k in feat else ""))

print()
print("=== deployable hybrids (per-function policy selector) ===")
# H1: gate-pass -> constrain-all ; gate-fail -> skip-xcall (recover gated-off funcs)
h1 = sum((prof[k][1] if gate(k) else xc[k][1]) for k in keys)
# H2: gate-pass -> constrain-all ; gate-fail -> skip-xcall ONLY if it shrinks (<0)
h2 = sum((prof[k][1] if gate(k) else min(0, xc[k][1])) for k in keys)
# H3: take the better of the two policies, but only among gate-pass funcs
print(f"  H1 gate?constrain-all:skip-xcall      net {h1:+7d}")
print(f"  H2 gate?constrain-all:min(0,skipxcall) net {h2:+7d}  (clamp: needs oracle sign)")

# Can a static feature pick the better policy per function?  Among funcs where
# the two differ, label = 1 if skip-xcall is better.  Test xcall_frac threshold.
diff = [k for k in keys if prof[k][1] != xc[k][1] and k in feat and feat[k]["ncand"]]
def betterskip(k): return xc[k][1] < prof[k][1]
print(f"\n  policy-selection by xcall_frac (funcs that differ: {len(diff)}):")
for thr in (0.05, 0.10, 0.15, 0.20, 0.30):
    # rule: if xcall_frac >= thr use skip-xcall, else constrain-all
    net = 0
    for k in keys:
        if k in feat and feat[k]["ncand"]:
            xf = feat[k]["xcall"]/feat[k]["ncand"]
            net += xc[k][1] if xf >= thr else prof[k][1]
        else:
            net += prof[k][1]
    print(f"    xcall_frac>={thr:.2f} -> skip-xcall : net {net:+7d}")
# combine with the call gate: gate-pass funcs additionally pick policy by xcall_frac
print(f"\n  call-gate + xcall_frac policy pick (gate-fail funcs dropped):")
for thr in (0.10, 0.15, 0.20):
    net = 0
    for k in keys:
        if not gate(k): continue
        fe = feat.get(k)
        if fe and fe["ncand"] and fe["xcall"]/fe["ncand"] >= thr:
            net += xc[k][1]
        else:
            net += prof[k][1]
    print(f"    gate & (xcall_frac>={thr:.2f}?skip:all) : net {net:+7d}")
