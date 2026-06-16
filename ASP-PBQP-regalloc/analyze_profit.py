#!/usr/bin/env python3
# Analyze the per-function profitability CSV (profitability.sh output).
#   python3 analyze_profit.py ~/profit.csv
#
# Answers: (1) what does the oracle look like -- how concentrated is the gain,
# how many functions help vs hurt; (2) can a cheap static rule on the features
# (maxlt / calls / call-density / cands ...) select the profitable functions;
# (3) the achievable net bytes if we applied narrowing only where a rule fires,
# vs the oracle ceiling (apply only where delta<0) and vs constrain-all.
import sys, csv, itertools

rows = []
with open(sys.argv[1]) as f:
    for r in csv.DictReader(f):
        try:
            rows.append({
                "bench": r["bench"], "func": r["func"],
                "base": int(r["base_bytes"]), "d": int(r["delta_bytes"]),
                "maxlt": int(r["maxlt"]), "pts": int(r["pts"]),
                "calls": int(r["calls"]), "cands": int(r["cands"]),
                "candrefs": int(r["candrefs"]), "blocks": int(r["blocks"]),
            })
        except (KeyError, ValueError):
            pass

n = len(rows)
allnet = sum(r["d"] for r in rows)
wins = [r for r in rows if r["d"] < 0]
losses = [r for r in rows if r["d"] > 0]
oracle = sum(r["d"] for r in rows if r["d"] < 0)  # apply only where it helps
print(f"functions: {n}   constrain-all net: {allnet:+d}")
print(f"  help (d<0): {len(wins)}  hurt (d>0): {len(losses)}  neutral: {n-len(wins)-len(losses)}")
print(f"  oracle ceiling (apply only where d<0): {oracle:+d}")
print(f"  total base text of all funcs: {sum(r['base'] for r in rows)}")
print()
print("top 12 winners:")
for r in sorted(rows, key=lambda x: x["d"])[:12]:
    cd = r["calls"]/r["pts"] if r["pts"] else 0
    print(f"  {r['d']:+6d}  {r['bench']:<14}{r['func']:<26} maxlt={r['maxlt']:<4} pts={r['pts']:<5} calls={r['calls']:<4} call/pt={cd:.3f} cands={r['cands']}")
print("top 12 losers:")
for r in sorted(rows, key=lambda x: -x["d"])[:12]:
    cd = r["calls"]/r["pts"] if r["pts"] else 0
    print(f"  {r['d']:+6d}  {r['bench']:<14}{r['func']:<26} maxlt={r['maxlt']:<4} pts={r['pts']:<5} calls={r['calls']:<4} call/pt={cd:.3f} cands={r['cands']}")
print()

def callpt(r):
    return r["calls"]/r["pts"] if r["pts"] else 0.0

# Evaluate a predicate: net bytes when applying narrowing only to funcs it
# selects, plus how many true winners it catches / losers it lets through.
def evalrule(name, pred):
    sel = [r for r in rows if pred(r)]
    net = sum(r["d"] for r in sel)
    sw = sum(1 for r in sel if r["d"] < 0); sl = sum(1 for r in sel if r["d"] > 0)
    caught = sum(r["d"] for r in sel if r["d"] < 0)
    leaked = sum(r["d"] for r in sel if r["d"] > 0)
    print(f"  {name:<34} sel={len(sel):<4} net={net:+7d}  (win {sw} / lose {sl}; "
          f"good {caught:+d}, bad {leaked:+d})")

print("rule sweeps (net = suite bytes if we narrow ONLY selected functions):")
print(" lower-bound on maxlt:")
for lo in (0, 10, 14, 18, 22, 28, 36):
    evalrule(f"maxlt>={lo}", lambda r, lo=lo: r["maxlt"] >= lo)
print(" band on maxlt:")
for lo, hi in [(14,50),(14,80),(18,60),(20,80),(28,80)]:
    evalrule(f"{lo}<=maxlt<={hi}", lambda r, lo=lo, hi=hi: lo <= r["maxlt"] <= hi)
print(" call-density (sparsity) gates:")
for lo, cp in [(14,0.05),(14,0.03),(18,0.05),(0,0.03)]:
    evalrule(f"maxlt>={lo} & call/pt<{cp}",
             lambda r, lo=lo, cp=cp: r["maxlt"] >= lo and callpt(r) < cp)
print(" size + contention:")
for lo, mp in [(14,200),(18,300),(14,400)]:
    evalrule(f"maxlt>={lo} & pts>={mp}",
             lambda r, lo=lo, mp=mp: r["maxlt"] >= lo and r["pts"] >= mp)
