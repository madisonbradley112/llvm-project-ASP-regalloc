%=============================================================================
% Per-program-point register allocation with live-range SPLITTING and spilling.
% Code-size objective.  Straight-line prototype: points 0..N along one block.
%
% This is the model `asp-splitting` exists to validate: unlike the whole-range
% single-phase model (which assigns a value one register for its whole life or
% spills it entirely), here a value's *location* is decided per program point,
% so the solver can keep a value in a register across its hot span and spill
% only the cold middle -- exactly what greedy's live-range splitting does, but
% chosen optimally and jointly with the compression objective.
%
% INPUT (per instance):
%   point(0..N).                 time steps (instruction boundaries)
%   reg(R).        gprc(R).       physical registers; gprc subset is compressible
%   live(V,P).                    value V is live at point P
%   use(V,P).      def(V,P).      V used / defined at P (must be in a register there)
%   cand(I,V,P).                  instr I at P uses V; compresses iff V is in a gprc reg
%   storecost(C). reloadcost(C). compsave(S).   byte costs / savings (objective weights)
%=============================================================================

% --- Decision: at each live point, V is in at most one register, else memory ---
{ inreg(V,P,R) : reg(R) } 1 :- live(V,P).
inmem(V,P) :- live(V,P), not inreg(V,P,_).

% --- One value per register per point (registers time-share across points) ---
:- inreg(V1,P,R), inreg(V2,P,R), V1 < V2.

% --- Uses and defs need the value in a register (no memory operands on RV) ---
:- use(V,P), inmem(V,P).
:- def(V,P), inmem(V,P).

% --- No free register-to-register teleporting: while a value stays continuously
%     in registers it keeps the SAME register.  To move registers it must pass
%     through memory (a store then a reload), which is costed below. ------------
:- inreg(V,P-1,R1), inreg(V,P,R2), R1 != R2.

% --- Transitions = the static spill instructions that get inserted -----------
%   reload: value enters a register from memory   (mem at P-1 -> reg at P)
%   store : value leaves a register to memory     (reg at P-1 -> mem at P)
reload(V,P) :- inreg(V,P,_), live(V,P-1), inmem(V,P-1).
store(V,P)  :- inmem(V,P),   live(V,P-1), inreg(V,P-1,_).

% --- Compression: a candidate is realized iff its value sits in a gprc reg ----
realized(I) :- cand(I,V,P), inreg(V,P,R), gprc(R).

% --- Objective: minimize spill bytes, maximize compression bytes saved -------
#maximize { S@1, comp,   I   : realized(I), cand(I,_,_), compsave(S) }.
#minimize { C@1, store,  V,P : store(V,P),  storecost(C) }.
#minimize { C@1, reload, V,P : reload(V,P), reloadcost(C) }.

#show inreg/3.
#show store/2.
#show reload/2.
#show realized/1.
