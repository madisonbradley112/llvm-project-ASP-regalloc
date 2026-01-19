% We represent the PBQP register allocation problem as an ASP problem.
% - Symbolic registers:  sym_register(Reg)
% - CPU registers:       cpu_register(Reg)
% - Assignments:         assign(SymReg, CPUReg)

#show assign/2.
% #show cost/2. 
#show total_cost/1.
#show spill_cost/3.
#show copy_propagation_cost/3.
#show param_copy_propagation_cost/4.
#show generic_constraint/4.


%=========== Spilling Costs ===========
% This is the cost incurred if a register s spills to memory.
% Specific register types require different spill costs. 

cpu_register(sp). % Spills are represented by an imaginary CPU register.

% If register S is assigned to spill, incur a spill cost
spill_cost(S, A, C) :- sym_register(S), cpu_register(A), 
                       assign(S,A), spill_cost(S, C), A == sp.


%=========== Interference Constraint ===========
% Indicates that two symbolic registers "interfere" together - that is, 
% they must be assigned to two different CPU registers.
% "Interfering" typically means that two variables are live at 
% the same time in a program. 
% In Scholz and Eckstein's work, this is represented as an infinity cost.
% We can represent it as a constraint. 

% symbolic registers S1 and S2 cannot interfere
% and be assigned to the same register
:- interferes(S1, S2),
   sym_register(S1), sym_register(S2), 
   cpu_register(A1),  cpu_register(A2),
   assign(S1, A1), assign(S2, A2), 
   A1 == A2, A1 != sp.

%=========== Class Constraint ===========
% Indicates that a symbolic register must be of the same class as its assigned CPU register.
% For example, floating point variables must be assigned to floating point registers.
% In Scholz and Eckstein's work, this is represented as an infinity cost
% if the register classes do not match.
% We can represent it as a constraint. 

% the class of symbolic register S must 
% match assigned cpu register A.
:- class(S, Class1), class(A, Class2),
   sym_register(S), 
   cpu_register(A), 
   assign(S, A),  
   Class1 != Class2, A != sp.

%=========== Copy Propagation Benefit ===========
% Copy propagation eliminates a copy instruction if source and target are assigned
% the same CPU register. This provides a possible benefit, represented by B. 
% We subtract B from the cost. 

% A benefit is available if A1 and A2 are the same, and A1 is not the spill register.
copy_propagation_cost(S1, S2, -B) :- 
    sym_register(S1), sym_register(S2), 
    cpu_register(A1), cpu_register(A2), 
    assign(S1, A1), assign(S2, A2), 
    copy_propagation_benefit(S1, S2, B), 
    A1 == A2, A1 != sp.

%=========== Parameter Copy Propagation Benefit ===========
% We can also do copy propagation for parameter passing where parameters are passed by registers.
% Here, one operand of the copy instruction is a CPU register. The copy instr can be eliminated
% if the symbolic register is assigned to the same CPU register in which the parameter is stored.

% A benefit is available if A and Rc are the same.
param_copy_propagation_cost(S, A, Rc, -B) :- 
    sym_register(S),
    cpu_register(A), 
    assign(S, A),
    param_copy_propagation_benefit(S, Rc, B), 
    cpu_register(Rc), A == Rc.

%=========== Shared Register Constraint ===========
% Irregular architectures often have shared (aka overlapped) registers, in which multiple registers
% share the same memory. It is not possible to use two registers that share memory
% at the same time.
:- sym_register(S1), sym_register(S2), 
    cpu_register(A1),  cpu_register(A2),
    assign(S1, A1), assign(S2, A2), 
    shared(A1, A2).

%=========== Generic Symbolic Register Dependency Constraint ===========
% Irregular architectures often compose constraints for instruction operands,
% to keep instruction words small. For example, in some DSP architectures, only some
% index registers are allowed in combination with an address register. 

% pairing is symmetric
pair(A, B) :- pair(B, A).

% if S1 and S2 follow a pair constraint, then A1 and A2 must be paired.
generic_constraint(S1, S2, A1, A2) :- 
    sym_register(S1), sym_register(S2), 
    cpu_register(A1),  cpu_register(A2),
    assign(S1, A1), assign(S2, A2), 
    is_paired(S1, S2),
    not pair(A1, A2).

% breaking the pair constraint is not permitted in an answer set
generic_constraint :- generic_constraint(S1, S2, A1, A2). 
:- generic_constraint.

% each symbolic register can only be assigned to one cpu register
:- assign(S, A1), assign(S, A2), A1 != A2.

% each symbolic register must have a cpu register assignment
:- sym_register(S), not assign(S, _).

% assign each symbolic register to a CPU register
1 { assign(S,A) : cpu_register(A) } 1 :- sym_register(S).

% the total cost of a symbolic assignment S is the sum of all
% costs associated with S. 
cost(C, S, spill) :- spill_cost(S, _, C).
cost(C, S, copy_propagation) :- copy_propagation_cost(S, _, C).
cost(C, S, param_propagation) :- param_copy_propagation_cost(S, _, _, C).

% get a *total* assignment cost over all assignments
total_cost(C) :- C = #sum { K, V, T : cost(K, V, T) }.

% now, we minimize costs over all register assignments
#minimize { C: total_cost(C)}.
