% Test 2: compression realized jointly with splitting.
% Same pressure-3 setup as Test 1, but reg 0 is a gprc register and v1's uses
% are compressible candidates (compress iff v1 sits in a gprc reg).  The solver
% should still split v0 across the busy middle AND keep v1 in the gprc reg 0 for
% its whole span so every candidate compresses.
% Expect: store(0,1)+reload(0,10) (cost 8), v1 in reg 0 throughout, all v1
% candidates realized (7 x 2 = 14 saved).  Optimization = 8 - 14 = -6.

#const n = 10.
point(0..n).
reg(0). gprc(0). reg(1).
storecost(4). reloadcost(4). compsave(2).

% v0: long range, endpoints only -> split candidate, not compressible.
live(0,0..n).  def(0,0).  use(0,n).

% v1: used throughout 1..9; each use (2..8) is a compression candidate.
live(1,1..9).  def(1,1).  use(1,P) :- live(1,P), P > 1.
cand((v1,P), 1, P) :- live(1,P), P >= 2, P <= 8.

% v2: used throughout 3..7 -> must stay resident, not compressible.
live(2,3..7).  def(2,3).  use(2,P) :- live(2,P), P > 3.
