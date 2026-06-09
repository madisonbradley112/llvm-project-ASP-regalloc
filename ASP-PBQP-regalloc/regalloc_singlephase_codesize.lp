% ===========================================================================
% Single-phase, code-size-optimal register allocation with RVC compression.
%
% Unlike the phase-1 hint model (regalloc_phase1_gprc.lp), this performs the
% WHOLE register allocation -- assignment AND spilling -- inside one ASP
% optimization whose objective is total code size in bytes.  Because spilling
% and compression are decided together, the model trades them directly: it puts
% a value in GPRC (enabling a 2-byte compression) only when doing so does not
% force a spill that costs more than it saves, and it will spill a cheap value
% to free a GPRC register when that *is* worth it.  This is the global tradeoff
% the decoupled phase-1+greedy design cannot make.
%
% Spill model: "spill everywhere", no live-range splitting -- a value is either
% held in one physical register for its whole live range, or kept in memory and
% loaded/stored at each use/def.  This over-approximates spill cost but keeps
% the problem decidable and is the standard combinatorial formulation
% (cf. Pereira & Palsberg; Castaneda Lozano & Schulte survey).
%
% ===========================================================================
% INPUT FACTS (emitted per function by the allocator front-end):
%
%   reg(R).                allocatable physical register (dense id).
%   gprc(R).               R is RVC-addressable (x8-x15).
%   callee_saved(R).       R is callee-saved (costs a prologue save/restore).
%
%   vreg(V).               a virtual register to allocate.
%   precolored(V, R).      V is ABI-fixed to physical register R.
%   interfere(V1, V2).     V1, V2 are simultaneously live (emit once, V1<V2).
%   spill_cost(V, C).      bytes of load/store code if V is spilled
%                          (#defs*store_bytes + #uses*reload_bytes, RVC-aware).
%
%   cand(I).               instruction I has a compressed form.
%   cand_save(I, B).       bytes saved if I is realized (typically 2).
%   needs_gprc(I, V).      operand V of I must be in GPRC for I to compress.
%   tied(V1, V2).          two-address pair: must share a register.
%
%   cs_cost(N).            bytes per callee-saved register used (RVC-aware).
% ===========================================================================

% ---- Decision: each vreg gets one physical register, or is spilled ---------
{ assign(V, R) : reg(R) } 1 :- vreg(V).
assigned(V) :- assign(V, _).
spilled(V)  :- vreg(V), not assigned(V).

% ABI-fixed values are pinned (and thus never spilled).
assign(V, R) :- precolored(V, R).

% ---- Core register-allocation constraints ----------------------------------
% No two simultaneously-live values may share a physical register.  When the
% interference graph is not colorable with the available registers, the choice
% rule above leaves some values unassigned -> spilled.  Capacity is implicit.
:- assign(V1, R), assign(V2, R), interfere(V1, V2).

% Two-address operands must share a register (or both be spilled).  A tied pair
% that interferes therefore cannot be co-located -> the candidate is blocked.
:- tied(V1, V2), assign(V1, R), not assign(V2, R).
:- tied(V1, V2), assign(V2, R), not assign(V1, R).

% ---- Compression realization -----------------------------------------------
% An operand is "GPRC-satisfied" when it is assigned to a GPRC register (a
% spilled or non-GPRC operand is not).
gprc_ok(V) :- assign(V, R), gprc(R).
blocked(I) :- needs_gprc(I, V), not gprc_ok(V).
realized(I) :- cand(I), not blocked(I).

% ---- Callee-saved usage ----------------------------------------------------
used_cs(R) :- assign(_, R), callee_saved(R).

% ---- Objective: minimize total code size (bytes) ---------------------------
% Equivalent to maximizing (compression bytes saved) minus (spill bytes) minus
% (callee-saved save/restore bytes).  All at one priority -- they are all bytes.
#maximize { B@1, comp, I  : realized(I), cand_save(I, B) }.
#minimize { C@1, spill, V : spilled(V), spill_cost(V, C) }.
#minimize { N@1, cs, R    : used_cs(R), cs_cost(N) }.

#show assign/2.
#show spilled/1.
#show realized/1.
