#!/usr/bin/env python3
# Mine the existing profit.csv for DERIVED features (no recompile).  For each
# raw + derived feature we (a) report Pearson corr with delta on active funcs,
# (b) sweep a threshold and report the best net-bytes split, and (c) try the
# best pair combined with the call-density gate.
import sys, csv, math

INTCOLS = ("base_bytes","delta_bytes","maxlt","pts","calls","cands","candrefs",
           "blocks","ncand","xcall","spanblk","clivelen")
rows = []
with open(sys.argv[1]) as f:
    for r in csv.DictReader(f):
        try:
            d = {k: int(r[k]) for k in INTCOLS if k in r}
            d["bench"], d["func"] = r["bench"], r["func"]
            rows.append(d)
        except (KeyError, ValueError):
            pass

HAVE_NEW = "xcall" in rows[0] if rows else False

def feats(r):
    pts = r["pts"] or 1; cands = r["cands"] or 1; blocks = r["blocks"] or 1
    f = {
        "maxlt":       r["maxlt"],
        "pts":         r["pts"],
        "calls":       r["calls"],
        "cands":       r["cands"],
        "candrefs":    r["candrefs"],
        "blocks":      r["blocks"],
        "callpt":      r["calls"]/pts,            # call density (current best gate)
        "cand_dens":   r["cands"]/pts,            # candidates per point
        "candref_dens":r["candrefs"]/pts,         # candidate refs per point
        "refs_per_cand": r["candrefs"]/cands,     # how heavily each cand is used
        "blk_size":    pts/blocks,                # avg instrs/block (straight-lineness)
        "cands_per_blk": r["cands"]/blocks,
        "calls_per_cand": r["calls"]/cands,       # call pressure relative to cands
        "lt_per_cand": r["maxlt"]/cands,          # contention vs cand supply
        "lt_x_blksz":  r["maxlt"]*(pts/blocks),   # contention in big blocks
    }
    if HAVE_NEW:
        nc = r["ncand"] or 1
        f.update({
            "ncand":        r["ncand"],
            "xcall":        r["xcall"],            # cands whose range crosses a call
            "spanblk":      r["spanblk"],          # cands living across block edges
            "clivelen":     r["clivelen"],         # total cand live length (slots)
            "xcall_frac":   r["xcall"]/nc,         # fraction of cands crossing calls
            "spanblk_frac": r["spanblk"]/nc,       # fraction spanning blocks
            "avglen":       r["clivelen"]/nc,      # avg cand live length
            "len_per_pt":   r["clivelen"]/pts,     # live length density
            "callfree":     1.0 - r["xcall"]/nc,   # fraction of cands NOT crossing calls
        })
    return f

# active = at least one candidate (only these can possibly change size)
act = [r for r in rows if r["cands"] > 0]
print(f"active funcs (cands>0): {len(act)} / {len(rows)}   "
      f"net over active: {sum(r['delta_bytes'] for r in act):+d}   "
      f"oracle: {sum(r['delta_bytes'] for r in act if r['delta_bytes']<0):+d}")
print()

def pearson(xs, ys):
    n = len(xs); mx = sum(xs)/n; my = sum(ys)/n
    num = sum((x-mx)*(y-my) for x,y in zip(xs,ys))
    dx = math.sqrt(sum((x-mx)**2 for x in xs)); dy = math.sqrt(sum((y-my)**2 for y in ys))
    return num/(dx*dy) if dx and dy else 0.0

names = list(feats(act[0]).keys())
ds = [r["delta_bytes"] for r in act]
print("Pearson(feature, delta) on active funcs  (negative => higher feature predicts SHRINK):")
corrs = []
for nm in names:
    xs = [feats(r)[nm] for r in act]
    c = pearson(xs, ds)
    corrs.append((nm, c))
for nm, c in sorted(corrs, key=lambda t: t[1]):
    print(f"  {nm:<16} r={c:+.3f}")
print()

# For each feature, sweep a threshold (both directions) and report the split
# that minimises net bytes among selected funcs.
def best_threshold(nm, direction):
    vals = sorted(set(feats(r)[nm] for r in act))
    best = (0, None, 0, 0)  # net, thr, win, lose
    for t in vals:
        if direction == "le":
            sel = [r for r in act if feats(r)[nm] <= t]
        else:
            sel = [r for r in act if feats(r)[nm] >= t]
        if not sel: continue
        net = sum(r["delta_bytes"] for r in sel)
        if net < best[0]:
            w = sum(1 for r in sel if r["delta_bytes"]<0)
            l = sum(1 for r in sel if r["delta_bytes"]>0)
            best = (net, t, w, l)
    return best

print("best single-feature threshold (net bytes if we narrow only selected):")
res = []
for nm in names:
    for direction in ("le","ge"):
        net,t,w,l = best_threshold(nm, direction)
        if t is not None:
            res.append((net, nm, direction, t, w, l))
for net,nm,direction,t,w,l in sorted(res)[:14]:
    op = "<=" if direction=="le" else ">="
    print(f"  {nm:<16}{op}{t:<10.4g} net={net:+7d}  (win {w} / lose {l})")
print()

# Two-feature: combine callpt<thr with each other feature's best direction.
print("pairs: callpt<0.03 AND <second feature>:")
base_sel = lambda r: feats(r)["callpt"] < 0.03
for nm in names:
    if nm == "callpt": continue
    vals = sorted(set(feats(r)[nm] for r in act))
    best = (0, None, "", 0, 0)
    for direction in ("le","ge"):
        for t in vals:
            if direction=="le":
                sel=[r for r in act if base_sel(r) and feats(r)[nm]<=t]
            else:
                sel=[r for r in act if base_sel(r) and feats(r)[nm]>=t]
            if not sel: continue
            net=sum(r["delta_bytes"] for r in sel)
            if net<best[0]:
                w=sum(1 for r in sel if r["delta_bytes"]<0)
                l=sum(1 for r in sel if r["delta_bytes"]>0)
                best=(net,t,direction,w,l)
    net,t,direction,w,l = best
    if t is not None:
        op="<=" if direction=="le" else ">="
        print(f"  callpt<0.03 & {nm:<14}{op}{t:<10.4g} net={net:+7d}  (win {w} / lose {l})")
