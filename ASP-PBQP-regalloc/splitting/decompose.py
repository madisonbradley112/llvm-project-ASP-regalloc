#!/usr/bin/env python3
# Region-decomposed solver for the per-point splitting model.
# Partitions the program points into consecutive windows of size W and solves
# each region left-to-right with clingo, freezing each region's entry boundary
# to the previous region's exit state.  Reports the stitched global objective
# and the total solve time, for comparison with the monolithic solve.
#
# Usage: decompose.py INSTANCE.lp WINDOW [PER_REGION_TIMELIMIT]
import sys, re, subprocess, time, os

inst_path = sys.argv[1]
W         = int(sys.argv[2])
tl        = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0
HERE      = os.path.dirname(os.path.abspath(__file__))
REGION_LP = os.path.join(HERE, "regalloc_region.lp")

txt = open(inst_path).read()

def find1(pat, default=None, cast=int):
    m = re.search(pat, txt)
    return cast(m.group(1)) if m else default

N    = find1(r"point\(0\.\.(\d+)\)") + 1
NR   = find1(r"reg\(0\.\.(\d+)\)") + 1
NG   = find1(r"gprc\(0\.\.(\d+)\)") + 1
SC   = find1(r"storecost\((\d+)\)", 4)
RC   = find1(r"reloadcost\((\d+)\)", 4)
CS   = find1(r"compsave\((\d+)\)", 2)

live = {}                                  # v -> (a,b)
for m in re.finditer(r"live\((\d+),(\d+)\.\.(\d+)\)", txt):
    v,a,b = map(int, m.groups()); live[v] = (a,b)
uses = {}                                  # v -> set of points
for m in re.finditer(r"use\((\d+),(\d+)\)", txt):
    v,p = map(int, m.groups()); uses.setdefault(v,set()).add(p)
defs = {}
for m in re.finditer(r"def\((\d+),(\d+)\)", txt):
    v,p = map(int, m.groups()); defs[v] = p
cands = []                                 # (id, v, p)
for m in re.finditer(r"cand\((\w+),(\d+),(\d+)\)", txt):
    cands.append((m.group(1), int(m.group(2)), int(m.group(3))))

header = [f"reg(0..{NR-1}).", f"gprc(0..{NG-1}).",
          f"storecost({SC}). reloadcost({RC}). compsave({CS})."]

def solve_region(lo, hi, frozen):
    """frozen: dict v -> ('reg',R) or ('mem',). Returns (atoms_text, secs)."""
    L = [f"point({lo}..{hi}).", f"first({lo})."] + header
    for v,(a,b) in live.items():
        s,e = max(a,lo), min(b,hi)
        if s <= e:
            L.append(f"live({v},{s}..{e}).")
            dp = defs.get(v)
            if dp is not None and lo <= dp <= hi:
                L.append(f"def({v},{dp}).")
            for p in uses.get(v, ()):
                if lo <= p <= hi:
                    L.append(f"use({v},{p}).")
    for cid,v,p in cands:
        if lo <= p <= hi:
            L.append(f"cand({cid},{v},{p}).")
    for v,st in frozen.items():
        if st[0] == 'reg': L.append(f"frozen_reg({v},{st[1]}).")
        else:              L.append(f"frozen_mem({v}).")
    reg_inst = f"/tmp/region_{lo}_{hi}.lp"
    open(reg_inst, "w").write("\n".join(L) + "\n")
    t0 = time.time()
    out = subprocess.run(
        ["clingo", REGION_LP, reg_inst, "--opt-mode=opt",
         f"--time-limit={int(tl)}", "--quiet=1,2"],
        capture_output=True, text=True).stdout
    return out, time.time() - t0

# Parse the last (best) answer's atoms from clingo output.
def parse_atoms(out):
    blocks = out.split("Answer:")
    if len(blocks) < 2:
        return None
    body = blocks[-1].split("\n")
    line = body[1] if len(body) > 1 else ""
    inreg = {}                              # (v,p) -> r
    stores = reloads = realized = 0
    for m in re.finditer(r"inreg\((\d+),(\d+),(\d+)\)", line):
        v,p,r = map(int, m.groups()); inreg[(v,p)] = r
    stores   = len(re.findall(r"store\(", line))
    reloads  = len(re.findall(r"reload\(", line))
    realized = len(re.findall(r"realized\(", line))
    return inreg, stores, reloads, realized

regions = [(lo, min(lo+W-1, N-1)) for lo in range(0, N, W)]
tot_store = tot_reload = tot_real = 0
tot_time = 0.0
exit_state = {}                             # v -> ('reg',R) or ('mem',)
ok = True
for lo, hi in regions:
    frozen = {}
    for v,(a,b) in live.items():
        if a < lo <= b and v in exit_state:   # crosses entry boundary
            frozen[v] = exit_state[v]
    out, secs = solve_region(lo, hi, frozen)
    tot_time += secs
    parsed = parse_atoms(out)
    if parsed is None:
        print(f"  region [{lo},{hi}]: NO MODEL"); ok = False; break
    inreg, st, rl, rz = parsed
    tot_store += st; tot_reload += rl; tot_real += rz
    # exit state at hi for values live at hi
    for v,(a,b) in live.items():
        if a <= hi <= b:
            r = inreg.get((v,hi))
            exit_state[v] = ('reg', r) if r is not None else ('mem',)

obj = tot_store*SC + tot_reload*RC - tot_real*CS
status = "ok" if ok else "INCOMPLETE"
print(f"DECOMP N={N} regs={NR} W={W} regions={len(regions)} | "
      f"obj={obj} (store={tot_store} reload={tot_reload} realized={tot_real}) | "
      f"time={tot_time:.2f}s | {status}")
