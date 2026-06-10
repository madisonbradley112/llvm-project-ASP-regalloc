#!/usr/bin/env python3
# Binary-search ("narrowing + re-widening") region-decomposed solver.
# For each region we find the LARGEST window that clingo proves optimal within
# the probe budget, via a binary search over [WMIN, Wmax] *seeded at the
# previous region's accepted width*:
#   - probe the seed; if it proves optimal, search UPWARD  [seed+1 .. Wmax]
#                       else                  search DOWNWARD[WMIN .. seed-1]
# The seed keeps probing cheap in stable (uniform-pressure) stretches, while the
# Wmax upper bound means that as soon as pressure eases the search jumps back to
# a wide window in O(log) probes -- i.e. it RE-WIDENS, not just narrows.  This
# matches the wide->narrow->wide pressure profile of a typical function
# (easy prologue, dense body, easy epilogue).
#
# Usage: bisect_decompose.py INSTANCE.lp [WMAX] [WMIN] [PROBE_SECS]
import sys, re, subprocess, time, os

inst_path = sys.argv[1]
WMAX  = int(sys.argv[2]) if len(sys.argv) > 2 else 40
WMIN  = int(sys.argv[3]) if len(sys.argv) > 3 else 5
PROBE = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
HERE  = os.path.dirname(os.path.abspath(__file__))
REGION_LP = os.path.join(HERE, "regalloc_region.lp")

txt = open(inst_path).read()
def find1(pat, d=None):
    m = re.search(pat, txt); return int(m.group(1)) if m else d
N  = find1(r"point\(0\.\.(\d+)\)") + 1
NR = find1(r"reg\(0\.\.(\d+)\)") + 1
NG = find1(r"gprc\(0\.\.(\d+)\)") + 1
SC = find1(r"storecost\((\d+)\)", 4)
RC = find1(r"reloadcost\((\d+)\)", 4)
CS = find1(r"compsave\((\d+)\)", 2)

live, uses, defs, cands = {}, {}, {}, []
for m in re.finditer(r"live\((\d+),(\d+)\.\.(\d+)\)", txt):
    v,a,b = map(int, m.groups()); live[v] = (a,b)
for m in re.finditer(r"use\((\d+),(\d+)\)", txt):
    v,p = map(int, m.groups()); uses.setdefault(v,set()).add(p)
for m in re.finditer(r"def\((\d+),(\d+)\)", txt):
    v,p = map(int, m.groups()); defs[v] = p
for m in re.finditer(r"cand\((\w+),(\d+),(\d+)\)", txt):
    cands.append((m.group(1), int(m.group(2)), int(m.group(3))))

header = [f"reg(0..{NR-1}).", f"gprc(0..{NG-1}).",
          f"storecost({SC}). reloadcost({RC}). compsave({CS})."]
PROBES = 0

def solve_region(lo, hi, frozen):
    global PROBES; PROBES += 1
    L = [f"point({lo}..{hi}).", f"first({lo})."] + header
    for v,(a,b) in live.items():
        s,e = max(a,lo), min(b,hi)
        if s <= e:
            L.append(f"live({v},{s}..{e}).")
            dp = defs.get(v)
            if dp is not None and lo <= dp <= hi: L.append(f"def({v},{dp}).")
            for p in uses.get(v, ()):
                if lo <= p <= hi: L.append(f"use({v},{p}).")
    for cid,v,p in cands:
        if lo <= p <= hi: L.append(f"cand({cid},{v},{p}).")
    for v,st in frozen.items():
        L.append(f"frozen_reg({v},{st[1]})." if st[0]=='reg' else f"frozen_mem({v}).")
    inst = f"/tmp/breg_{lo}_{hi}.lp"; open(inst,"w").write("\n".join(L)+"\n")
    out = subprocess.run(
        ["clingo", REGION_LP, inst, "--opt-mode=opt",
         f"--time-limit={max(1,int(round(PROBE)))}", "--quiet=1,2"],
        capture_output=True, text=True).stdout
    optimal = "OPTIMUM FOUND" in out
    blocks = out.split("Answer:")
    inreg, st, rl, rz = {}, 0, 0, 0
    if len(blocks) >= 2:
        ls = blocks[-1].split("\n"); line = ls[1] if len(ls) > 1 else ""
        for m in re.finditer(r"inreg\((\d+),(\d+),(\d+)\)", line):
            v,p,r = map(int, m.groups()); inreg[(v,p)] = r
        st = len(re.findall(r"store\(", line)); rl = len(re.findall(r"reload\(", line))
        rz = len(re.findall(r"realized\(", line))
    return (inreg, st, rl, rz), optimal

def best_region(lo, frozen, seed):
    """Largest width in [WMIN, Wcap] proven optimal, binary-searched from seed."""
    Wcap = min(WMAX, N - lo)
    seed = max(WMIN, min(seed, Wcap))
    res, opt = solve_region(lo, lo+seed-1, frozen)
    if opt:
        best_w, best = seed, res
        loB, hiB = seed+1, Wcap                  # expand upward (re-widen)
        while loB <= hiB:
            mid = (loB+hiB)//2
            r, o = solve_region(lo, lo+mid-1, frozen)
            if o: best_w, best = mid, r; loB = mid+1
            else: hiB = mid-1
        return best_w, best
    loB, hiB = WMIN, seed-1                       # contract downward (narrow)
    best_w, best = None, None
    while loB <= hiB:
        mid = (loB+hiB)//2
        r, o = solve_region(lo, lo+mid-1, frozen)
        if o: best_w, best = mid, r; loB = mid+1
        else: hiB = mid-1
    if best_w is None:                            # even WMIN times out: best-effort
        best_w, best = min(WMIN, Wcap), solve_region(lo, lo+min(WMIN,Wcap)-1, frozen)[0]
    return best_w, best

tot_s = tot_r = tot_z = 0
seq = []
t0 = time.time()
exit_state = {}
lo, seed = 0, WMAX
while lo < N:
    frozen = {v:exit_state[v] for v,(a,b) in live.items() if a < lo <= b and v in exit_state}
    w, (inreg, st, rl, rz) = best_region(lo, frozen, seed)
    seq.append(w); seed = w
    tot_s += st; tot_r += rl; tot_z += rz
    hi = lo + w - 1
    for v,(a,b) in live.items():
        if a <= hi <= b:
            r = inreg.get((v,hi)); exit_state[v] = ('reg',r) if r is not None else ('mem',)
    lo = hi + 1

secs = time.time() - t0
obj = tot_s*SC + tot_r*RC - tot_z*CS
print(f"BISECT N={N} regs={NR} Wmax={WMAX} probe={PROBE}s | obj={obj} "
      f"(store={tot_s} reload={tot_r} realized={tot_z}) | time={secs:.2f}s | "
      f"regions={len(seq)} probes={PROBES} ({PROBES/len(seq):.1f}/region)")
print(f"  width sequence: {seq}")
