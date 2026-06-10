%=============================================================================
% Region (windowed) per-point allocation model — one region of a decomposition.
% Identical to regalloc_splitting.lp on the region's interior, plus BOUNDARY
% handling at the region's first point `first(Lo)`: values that crossed into
% this region carry their location from the previous region's exit as
%   frozen_reg(V,R).   % V was in register R at the previous region's last point
%   frozen_mem(V).     % V was in memory there
% Changing that location at the boundary costs a store/reload, exactly as an
% interior transition would; staying put is free.  Solving regions left-to-right
% with each region frozen to the previous one's exit stitches the pieces into a
% globally consistent allocation.
%
% INPUT (per region): point(Lo..Hi). first(Lo). reg/gprc/live/use/def/cand,
%   storecost/reloadcost/compsave, and frozen_reg/frozen_mem for crossing values.
%=============================================================================

{ inreg(V,P,R) : reg(R) } 1 :- live(V,P).
inmem(V,P) :- live(V,P), not inreg(V,P,_).

:- inreg(V1,P,R), inreg(V2,P,R), V1 < V2.

:- use(V,P), inmem(V,P).
:- def(V,P), inmem(V,P).

% Interior continuity: a value staying in registers keeps the same register.
:- inreg(V,P-1,R1), inreg(V,P,R2), R1 != R2.

% Interior transitions (reg<->mem) = static spill instructions.
reload(V,P) :- inreg(V,P,_), live(V,P-1), inmem(V,P-1).
store(V,P)  :- inmem(V,P),   live(V,P-1), inreg(V,P-1,_).

% --- Boundary at the region's first point Lo (uses frozen prev-exit state) ---
% Continuity across the boundary: same register, or go through memory.
:- inreg(V,Lo,R2), first(Lo), frozen_reg(V,R1), R1 != R2.
% A crossing value reloaded into a register here (it was in memory before).
reload(V,Lo) :- inreg(V,Lo,_), first(Lo), frozen_mem(V).
% A crossing value dropped to memory here (it was in a register before).
store(V,Lo)  :- inmem(V,Lo),  first(Lo), frozen_reg(V,_).

% --- Compression ---
realized(I) :- cand(I,V,P), inreg(V,P,R), gprc(R).

#maximize { S@1, comp,   I   : realized(I), cand(I,_,_), compsave(S) }.
#minimize { C@1, store,  V,P : store(V,P),  storecost(C) }.
#minimize { C@1, reload, V,P : reload(V,P), reloadcost(C) }.

#show inreg/3.
#show store/2.
#show reload/2.
#show realized/1.
