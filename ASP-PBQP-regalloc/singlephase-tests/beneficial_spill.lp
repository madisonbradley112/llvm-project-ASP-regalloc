% Same pressure (3 values, 2 regs, 1 must spill) but now the candidate v0 is
% expensive to spill (10) and there is a near-free value n1 (1 byte) to spill
% instead. The size-optimal move is to spill n1 and KEEP v0 compressed.
reg(0;1). gprc(0).
vreg(v0;n1;n2).
cand(i0). cand_save(i0,2). needs_gprc(i0,v0).
interfere(v0,n1). interfere(v0,n2). interfere(n1,n2).
spill_cost(v0,10). spill_cost(n1,1). spill_cost(n2,10).
cs_cost(4).
