% ==== REGISTER ASSIGNMENTS =======
% Symbolic registers from the Scholz-Eckstein example:
%   sa* = address class, sn* = index class,
%   sf* = short-float class, sfl* = long-float class
sym_register(sa0; sa1; sa2; sn0; sn1; sn2; sn3; sf1; sf2; sfl0).
% CPU registers for each class, plus the imaginary spill register sp
cpu_register(a0; a1; a2; a3; n0; n1; fl0; fl1; f0; f1; f2; f3).



% ==== REGISTER CLASSES =======
class(sa0, address; sa1, address; sa2, address;).
class(sn0, index; sn1, index; sn2, index; sn3, index).
class(sf1, short_float; sf2, short_float).
class(sfl0, long_float).

class(a0, address; a1, address; a2, address; a3, address).
class(n0, index; n1, index).
class(f0, short_float; f1, short_float; f2, short_float; f3, short_float).
class(fl0, long_float; fl1, long_float).

% ==== COST FUNCTIONS ======
interferes(sa0, sa2).
interferes(sa0, sa1).
interferes(sa1, sa2).
interferes(sf1, sf2). 

spill_cost(sa0, 110).
spill_cost(sa1, 110).
spill_cost(sa2, 110). 
spill_cost(sn0, 110).
spill_cost(sn1, 100).
spill_cost(sn2, 100). 
spill_cost(sn3, 100). 
spill_cost(sfl0, 220).
spill_cost(sf1, 200).
spill_cost(sf2, 200).

copy_propagation_benefit(sn1, sn0, 50).
copy_propagation_benefit(sn2, sn0, 50).
copy_propagation_benefit(sn3, sn0, 50).

param_copy_propagation_benefit(sa2, a2, 5).

shared(fl0, f0).
shared(fl0, f1). 
shared(fl1, f2). 
shared(fl1, f3).

is_paired(sa0, sn1).
is_paired(sa1, sn2). 
is_paired(sa2, sn3).
pair(sp, a0; sp, a1; sp, a2; sp, a3;). 
pair(n0, sp; n0, a0; n0, a1;).
pair(n1, sp; n1, a2; n1, a3;).