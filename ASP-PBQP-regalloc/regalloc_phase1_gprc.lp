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
gprc_vreg(V) :- needs_gprc(_, V), not precolored(V, _).

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

% ---- Realization of compression -------------------------------------------
blocked(I)  :- needs_gprc(I, V), not in_gprc(V).
realized(I) :- cand(I), not blocked(I).
demoted(I)  :- cand(I), not realized(I).

% ---- Objective: maximize bytes saved by realized compressions -------------
#maximize { B@1, I : realized(I), cand_saving(I, B) }.

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
