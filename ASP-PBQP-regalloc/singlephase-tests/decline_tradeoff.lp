% 2 registers (r0 = GPRC, r1 = non-GPRC). Three mutually-live values force
% exactly one spill. The candidate v0 is cheap to spill (2 bytes); the two
% non-candidates are expensive (10 bytes each).
reg(0;1). gprc(0).
vreg(v0;n1;n2).
cand(i0). cand_save(i0,2). needs_gprc(i0,v0).
interfere(v0,n1). interfere(v0,n2). interfere(n1,n2).
spill_cost(v0,2). spill_cost(n1,10). spill_cost(n2,10).
cs_cost(4).
