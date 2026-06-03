% phase1_example.lp -- tiny demonstration scenario for regalloc_phase1_gprc.lp
%
% Run with:
%   clingo regalloc_phase1_gprc.lp phase1_example.lp
%
% Demo machine: the GPRC pool is shrunk to 3 registers (the real RVC class is
% x8..x15) so that register pressure forces a non-trivial choice in this small
% example. Swap in x8..x15 for realistic experiments.
gprc_reg(x8;x9;x10).

% Five compression-candidate vregs, all simultaneously live (a 5-clique):
% only 3 of them can be pinned into the 3 GPRC registers at once.
cvreg(v1;v2;v3;v4;v5).
interfere(A, B) :- cvreg(A), cvreg(B), A < B.

% Five GPRC-requiring candidates arranged as a dependency cycle: each needs
% two adjacent vregs in GPRC (think c.lw / c.sw / c.and / c.sub / c.xor).
cand(i1). cand_saving(i1, 2). needs_gprc(i1, v1). needs_gprc(i1, v2).
cand(i2). cand_saving(i2, 2). needs_gprc(i2, v2). needs_gprc(i2, v3).
cand(i3). cand_saving(i3, 2). needs_gprc(i3, v3). needs_gprc(i3, v4).
cand(i4). cand_saving(i4, 2). needs_gprc(i4, v4). needs_gprc(i4, v5).
cand(i5). cand_saving(i5, 2). needs_gprc(i5, v5). needs_gprc(i5, v1).

% Expected optimum: the candidate graph is a 5-cycle, which has no triangle,
% so any 3 pinned vregs realize at most 2 candidates (4 bytes). The other 3
% candidates are demoted to their 32-bit form.
