% Phase-1 register allocation: GPRC (x8-x15) assignment to maximize RVC
% compression.
%
% This solves ONLY the scarce-resource part of the problem: which virtual
% registers should occupy the 8 RVC-addressable registers x8-x15 (the GPRC
% class), so that as many compression candidates as possible are realized.
% Everything not pinned here is left to LLVM's greedy allocator (phase 2),
% which also handles any true stack spilling.
%
% A candidate instruction that cannot get all of its required operands into
% GPRC is simply not realized -> it stays in its 32-bit form ("demoted").
% No explicit spill machinery is needed: demotion is the absence of a win.
%
% ===========================================================================
% INPUT INTERFACE (facts produced upstream from LLVM LiveIntervals + the
% compression rules in regalloc_compressed_ISA.lp):
%
%   gprc_reg(R).            a physical register in the GPRC class.
%                           Real target: x8;x9;x10;x11;x12;x13;x14;x15.
%
%   cand(I).                instruction I matches a GPRC-requiring compression
%                           pattern (opcode/immediate/target checks already
%                           passed; only the register-class placement is open).
%   cand_saving(I, Bytes).  bytes saved if I is realized as compressed (=2).
%   needs_gprc(I, V).       operand vreg V of I must be in GPRC for I to compress.
%
%   tied(V1, V2).           V1 and V2 are a two-address pair (rd == rs1 in the
%                           pattern) and must share one physical register.
%   interfere(V1, V2).      V1 and V2 are simultaneously live (given once, with
%                           V1 < V2). Tied pairs must NOT appear here.
%   precolored(V, R).       V is fixed to physical register R (e.g. sp, x0, x1).
%
% Note: instructions whose compressed form uses GPRNoX0/GPR (c.lwsp, c.swsp,
% c.mv, c.add, c.lui, ...) do NOT constrain the scarce GPRC pool and are left
% entirely to phase 2 -- they should not be emitted as cand/needs_gprc here.
% ===========================================================================

% ---- Derived sets ---------------------------------------------------------
% A vreg is a free GPRC-placement candidate unless it is ABI-pinned: either to
% a GPRC physreg (precolored/2, fixed there) or to a non-GPRC physreg
% (pinned_nongprc/1, can never be GPRC, so its candidates are blocked).
gprc_vreg(V) :- needs_gprc(_, V), not precolored(V, _), not pinned_nongprc(V).

tied_sym(A, B) :- tied(A, B).
tied_sym(A, B) :- tied(B, A).

% ---- Decision: each GPRC-candidate vreg gets at most one GPRC register -----
{ in_gprc(V, R) : gprc_reg(R) } 1 :- gprc_vreg(V).
in_gprc(V) :- in_gprc(V, _).

% Pre-colored operands that already sit in a GPRC physical register are fixed.
in_gprc(V, R) :- precolored(V, R), gprc_reg(R).

% ---- Constraints ----------------------------------------------------------
% No two interfering vregs may occupy the same GPRC register.
:- in_gprc(V1, R), in_gprc(V2, R), interfere(V1, V2).

% Two-address operands are coalesced: they share one register, hence either
% both are placed in the same GPRC reg or neither is placed.
:- tied_sym(V1, V2), in_gprc(V1, R), not in_gprc(V2, R).

% A vreg live across a call cannot occupy a caller-saved GPRC register
% (x10-x15 are clobbered at the call); only callee-saved GPRC (x8/x9) survives.
% Without this, binding such a placement would force a spill/reload around the
% call.  live_across_call/1 is supplied for the relevant vregs.
:- in_gprc(V, R), live_across_call(V), gprc_reg(R), not gprc_callee_saved(R).

% GPRC-occupancy headroom: at each high-pressure program point P, allow at most
% gprc_cap(K) candidates in GPRC, leaving the rest of x8-x15 for values not
% modelled here (addresses, non-candidates).  point/1 and at/2 give the
% candidate vregs live at P; the pass emits them only for points over capacity.
:- point(P), gprc_cap(K), C = #count { V : in_gprc(V), at(P, V) }, C > K.

% ---- Realization of compression -------------------------------------------
blocked(I)  :- needs_gprc(I, V), not in_gprc(V).
realized(I) :- cand(I), not blocked(I).
demoted(I)  :- cand(I), not realized(I).

% ---- Callee-saved cost ----------------------------------------------------
% A callee-saved GPRC register (x8/x9 under the standard ABI) costs a prologue
% save/restore. gprc_callee_saved/1 marks those registers; cs_surcharge/1 and
% cs_per_reg/1 give the spill cost in bytes (same unit as cand_saving):
%   * cs_surcharge -- paid once if ANY callee-saved GPRC register is touched
%     (the frame setup, or the call/tail into save-restore millicode).
%   * cs_per_reg    -- paid for EACH callee-saved register (the save/restore
%     pair under an inline frame; ~0 under save-restore/Zcmp where a single
%     pushed/popped list amortizes the whole range).
% The upstream pass supplies these as facts per the active spill mechanism:
%   inline frame    -> cs_surcharge(4). cs_per_reg(4).   (first reg = 8 bytes)
%   save-restore    -> cs_surcharge(8). cs_per_reg(0).
%   Zcmp push/pop   -> cs_surcharge(2). cs_per_reg(0).
% For standalone runs of this file, edit the two facts below to taste.
cs_surcharge(4).
cs_per_reg(4).

used_callee_saved(R) :- in_gprc(_, R), gprc_callee_saved(R).
any_callee_saved     :- used_callee_saved(_).

% ---- Objective ------------------------------------------------------------
% Single @2 level: maximize realized compression bytes minus the true spill
% cost of any callee-saved GPRC registers touched, so a candidate is realized
% only when its saving exceeds the spill it forces.
#maximize { B@2, realized, I : realized(I), cand_saving(I, B) }.
#minimize { S@2, surcharge : any_callee_saved, cs_surcharge(S) }.
#minimize { C@2, perreg, R : used_callee_saved(R), cs_per_reg(C) }.
% Per-candidate spill cost: placing a vreg in GPRC inside a register-saturated
% region forces a spill of some value.  cand_spill_cost(V, S) gives that cost in
% bytes (supplied by the pass for vregs whose live range crosses a high-pressure
% region), netted against the compression so a placement happens only when the
% bytes it saves exceed the spill bytes it induces.
#minimize { S@2, spill, V : in_gprc(V), cand_spill_cost(V, S) }.

% ---- Optional pressure guardrail (disabled by default) --------------------
% Pinning a vreg into GPRC removes one register from the GPR pool greedy sees,
% so over-pinning can force real stack spills in phase 2 that cost more than
% the 2 bytes saved here. If you have a per-program-point GPR pressure model,
% cap the number of simultaneously-pinned vregs to leave greedy headroom:
%
%   #const max_pins = 8.
%   :- #count { V : in_gprc(V) } > max_pins.

#show in_gprc/2.
#show realized/1.
#show demoted/1.
