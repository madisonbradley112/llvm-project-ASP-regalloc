#!/usr/bin/env python3
# Adaptive ("narrowing") region-decomposed solver for the per-point splitting
# model.  From each position we try the WIDEST window first; if clingo proves
# the region optimal within the probe budget we accept it (wider windows mean
# fewer seams and less boundary loss).  If the region only times out to a
# non-proven SAT, we halve the window and retry, narrowing until it proves
# optimum or hits a floor.  This uses wide windows where the program is easy
# (low pressure) and narrows automatically where it is hard -- no hand-tuned W.
#
# Usage: adaptive_decompose.py INSTANCE.lp [WMAX] [WMIN] [PROBE_SECS]
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

def solve_region(lo, hi, frozen, tl):
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
    inst = f"/tmp/areg_{lo}_{hi}.lp"; open(inst,"w").write("\n".join(L)+"\n")
    out = subprocess.run(
        ["clingo", REGION_LP, inst, "--opt-mode=opt",
         f"--time-limit={max(1,int(round(tl)))}", "--quiet=1,2"],
        capture_output=True, text=True).stdout
    optimal = "OPTIMUM FOUND" in out
    blocks = out.split("Answer:")
    inreg, st, rl, rz = {}, 0, 0, 0
    if len(blocks) >= 2:
        line = blocks[-1].split("\n")[1] if len(blocks[-1].split("\n"))>1 else ""
        for m in re.finditer(r"inreg\((\d+),(\d+),(\d+)\)", line):
            v,p,r = map(int, m.groups()); inreg[(v,p)] = r
        st = len(re.findall(r"store\(", line)); rl = len(re.findall(r"reload\(", line))
        rz = len(re.findall(r"realized\(", line))
    return inreg, st, rl, rz, optimal

tot_s = tot_r = tot_z = 0
widths, narrowings = [], 0
t0 = time.time()
exit_state = {}
lo = 0
prev_w = WMAX                     # "breathing" start: begin near the last
while lo < N:                     # accepted width, grown 50%, so sustained hard
    frozen = {v:exit_state[v] for v,(a,b) in live.items() if a < lo <= b and v in exit_state}
    # stretches don't re-probe WMAX every region; easy stretches widen back out.
    w = min(WMAX, N - lo, max(WMIN, prev_w + prev_w // 2))
    chosen = None
    while True:
        hi = lo + w - 1
        inreg, st, rl, rz, opt = solve_region(lo, hi, frozen, PROBE)
        if opt or w <= WMIN:
            chosen = (hi, inreg, st, rl, rz, opt, w); break
        narrowings += 1
        w = max(WMIN, w // 2)
    hi, inreg, st, rl, rz, opt, w = chosen
    prev_w = w
    widths.append((w, opt))
    tot_s += st; tot_r += rl; tot_z += rz
    for v,(a,b) in live.items():
        if a <= hi <= b:
            r = inreg.get((v,hi)); exit_state[v] = ('reg',r) if r is not None else ('mem',)
    lo = hi + 1

secs = time.time() - t0
obj = tot_s*SC + tot_r*RC - tot_z*CS
nopt = sum(1 for _,o in widths if o)
wd = {}
for w,_ in widths: wd[w] = wd.get(w,0)+1
hist = " ".join(f"{w}x{c}" for w,c in sorted(wd.items(), reverse=True))
print(f"ADAPT N={N} regs={NR} Wmax={WMAX} probe={PROBE}s | obj={obj} "
      f"(store={tot_s} reload={tot_r} realized={tot_z}) | time={secs:.2f}s | "
      f"regions={len(widths)} optimal={nopt}/{len(widths)} narrowings={narrowings} | "
      f"widths[{hist}]")
