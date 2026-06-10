% Test 1: splitting must beat whole-range spilling.
% 2 registers.  v0 spans the whole block but is used only at its endpoints;
% v1 and v2 are used at every point of their (overlapping) ranges, so they
% cannot be spilled cheaply.  At points 3..7 all three are live (pressure 3 >
% 2 regs), so SOMETHING must leave a register there.  The unique cheap answer
% is to split v0: keep it in a register at point 0, store it, leave it in
% memory across the busy middle, reload it before point 10.  Cost = 1 store +
% 1 reload.  Spilling any part of v1/v2 would cost more (they are used at
% consecutive points).  Expect: store(0,1), reload(0,10); no spills of v1/v2.

#const n = 10.
point(0..n).
reg(0). reg(1).
storecost(4). reloadcost(4). compsave(2).

% v0: long range, used only at endpoints -> ideal split candidate.
live(0,0..n).  def(0,0).  use(0,n).

% v1: live 1..9, used throughout -> must stay in a register.
live(1,1..9).  def(1,1).  use(1,P) :- live(1,P), P > 1.

% v2: live 3..7, used throughout -> must stay in a register.
live(2,3..7).  def(2,3).  use(2,P) :- live(2,P), P > 3.
