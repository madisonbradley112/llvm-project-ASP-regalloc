% RISC-V "C" (Compressed) Extension Compressibility
%
% This file encodes the RISC-V compression patterns ("CompressPat" records)
% from llvm/lib/Target/RISCV/RISCVInstrInfoC.td as ASP rules.
%
% A `compressed_instruction(Name, ...Operands..., 16, Counter)` fact is derived
% whenever a full-width `instruction(...)` fact at program point `Counter`
% satisfies the operand/register-class/immediate/target constraints that the
% .td CompressPat requires. The `16` is the compressed encoding width in bits
% (vs. the 32-bit base instruction).
%
% ---------------------------------------------------------------------------
% Expected EDB (facts supplied by the instruction-stream generator):
%
%   instruction(Op, A, B, C, Cost, Counter)  base instruction at point Counter.
%       - I-type / load:  (Op, Rd,  Rs1, Imm, Cost, Counter)
%       - store:          (Op, Rs2, Rs1, Imm, Cost, Counter)   (Rs2 = data reg)
%       - R-type:         (Op, Rd,  Rs1, Rs2, Cost, Counter)
%   instruction(jal,  Rd, Offset, Cost, Counter)                 (5-ary)
%   instruction(Op, Cost, Counter)         operand-less (ebreak, unimp)
%
%   in_class(Reg, Class)   register Reg belongs to register class Class.
%   signed(Imm) / unsigned(Imm)   signedness of the immediate field.
%   length(Imm, N)         Imm is representable in N bits (per its signedness).
%   lsb(Imm, K)            the K least-significant bits of Imm are zero.
%   nonzero(Imm) / zero(Imm)
%   clui_imm(Imm)          Imm is a legal c.lui immediate (see note below).
%   extension(c|d|f)       the named standard extension is enabled.
%   isRV32 / isRV64        the target XLEN.
% ---------------------------------------------------------------------------


% --- Register classes ------------------------------------------------------
% Note: x0, x1 and sp(=x2) are modelled as singleton physical-register classes
% so the same in_class/2 mechanism handles both class- and exact-register
% constraints (this mirrors how the original file used SP).
register_class(gprc).        % 8 regs (x8-x15), addressable by 3-bit fields
register_class(gprnox0).     % GPR except x0
register_class(gprnox0x2).   % GPR except x0 and x2
register_class(gpr).         % full integer register file
register_class(fpr64c).      % compressed FP-double class
register_class(fpr32c).      % compressed FP-single class
register_class(fpr64).       % FP-double
register_class(fpr32).       % FP-single
register_class(sp).          % x2
register_class(x0).          % zero register
register_class(x1).          % return address (ra)


% --- Immediate operand classes (mirror the AsmOperand classes in the .td) ---
uimm7_lsb00(Imm)            :- unsigned(Imm), length(Imm, 7),  lsb(Imm, 2).
uimm8_lsb00(Imm)            :- unsigned(Imm), length(Imm, 8),  lsb(Imm, 2).
uimm8_lsb000(Imm)           :- unsigned(Imm), length(Imm, 8),  lsb(Imm, 3).
uimm9_lsb000(Imm)           :- unsigned(Imm), length(Imm, 9),  lsb(Imm, 3).
uimm10_lsb00nonzero(Imm)    :- unsigned(Imm), length(Imm, 10), lsb(Imm, 2), nonzero(Imm).

simm6(Imm)                  :- signed(Imm), length(Imm, 6).
simm6nonzero(Imm)           :- signed(Imm), length(Imm, 6), nonzero(Imm).
simm9_lsb0(Imm)             :- signed(Imm), length(Imm, 9),  lsb(Imm, 1).
simm10_lsb0000nonzero(Imm)  :- signed(Imm), length(Imm, 10), lsb(Imm, 4), nonzero(Imm).
simm12_lsb0(Imm)            :- signed(Imm), length(Imm, 12), lsb(Imm, 1).

% Shift amount (uimmlog2xlennonzero): 5 bits on RV32, 6 bits on RV64, nonzero.
shamt(Imm) :- unsigned(Imm), nonzero(Imm), length(Imm, 5), isRV32.
shamt(Imm) :- unsigned(Imm), nonzero(Imm), length(Imm, 6), isRV64.

% c_lui_imm is nonzero and lies in [1,31] or in the sign-extended high range
% [0xfffe0, 0xfffff]. That disjoint range is not expressible via length/lsb,
% so it is taken as a primitive predicate clui_imm/1 from the generator.


%===========================================================================%
% Quadrant 0                                                                %
%===========================================================================%

% ADDI rd, sp, imm                          -> c.addi4spn   [C]
compressed_instruction(c_addi4spn, Rd, Rs1, Imm, 16, Counter) :-
    instruction(addi, Rd, Rs1, Imm, _, Counter),
    in_class(Rd, gprc), in_class(Rs1, sp),
    uimm10_lsb00nonzero(Imm),
    extension(c).

% FLD rd, rs1, imm                          -> c.fld        [C, D]
compressed_instruction(c_fld, Rd, Rs1, Imm, 16, Counter) :-
    instruction(fld, Rd, Rs1, Imm, _, Counter),
    in_class(Rd, fpr64c), in_class(Rs1, gprc),
    uimm8_lsb000(Imm),
    extension(c), extension(d).

% LW rd, rs1, imm                           -> c.lw         [C]
compressed_instruction(c_lw, Rd, Rs1, Imm, 16, Counter) :-
    instruction(lw, Rd, Rs1, Imm, _, Counter),
    in_class(Rd, gprc), in_class(Rs1, gprc),
    uimm7_lsb00(Imm),
    extension(c).

% FLW rd, rs1, imm                          -> c.flw        [C, F, RV32]
compressed_instruction(c_flw, Rd, Rs1, Imm, 16, Counter) :-
    instruction(flw, Rd, Rs1, Imm, _, Counter),
    in_class(Rd, fpr32c), in_class(Rs1, gprc),
    uimm7_lsb00(Imm),
    extension(c), extension(f), isRV32.

% LD rd, rs1, imm                           -> c.ld         [C, RV64]
compressed_instruction(c_ld, Rd, Rs1, Imm, 16, Counter) :-
    instruction(ld, Rd, Rs1, Imm, _, Counter),
    in_class(Rd, gprc), in_class(Rs1, gprc),
    uimm8_lsb000(Imm),
    extension(c), isRV64.

% FSD rs2, rs1, imm                         -> c.fsd        [C, D]
compressed_instruction(c_fsd, Rs2, Rs1, Imm, 16, Counter) :-
    instruction(fsd, Rs2, Rs1, Imm, _, Counter),
    in_class(Rs2, fpr64c), in_class(Rs1, gprc),
    uimm8_lsb000(Imm),
    extension(c), extension(d).

% SW rs2, rs1, imm                          -> c.sw         [C]
compressed_instruction(c_sw, Rs2, Rs1, Imm, 16, Counter) :-
    instruction(sw, Rs2, Rs1, Imm, _, Counter),
    in_class(Rs2, gprc), in_class(Rs1, gprc),
    uimm7_lsb00(Imm),
    extension(c).

% FSW rs2, rs1, imm                         -> c.fsw        [C, F, RV32]
compressed_instruction(c_fsw, Rs2, Rs1, Imm, 16, Counter) :-
    instruction(fsw, Rs2, Rs1, Imm, _, Counter),
    in_class(Rs2, fpr32c), in_class(Rs1, gprc),
    uimm7_lsb00(Imm),
    extension(c), extension(f), isRV32.

% SD rs2, rs1, imm                          -> c.sd         [C, RV64]
compressed_instruction(c_sd, Rs2, Rs1, Imm, 16, Counter) :-
    instruction(sd, Rs2, Rs1, Imm, _, Counter),
    in_class(Rs2, gprc), in_class(Rs1, gprc),
    uimm8_lsb000(Imm),
    extension(c), isRV64.


%===========================================================================%
% Quadrant 1                                                                %
%===========================================================================%

% ADDI x0, x0, 0                            -> c.nop        [C]
compressed_instruction(c_nop, 16, Counter) :-
    instruction(addi, Rd, Rd, Imm, _, Counter),
    in_class(Rd, x0), zero(Imm),
    extension(c).

% ADDI rs1, rs1, imm   (rd == rs1)          -> c.addi       [C]
compressed_instruction(c_addi, Rd, Imm, 16, Counter) :-
    instruction(addi, Rd, Rd, Imm, _, Counter),
    in_class(Rd, gprnox0),
    simm6nonzero(Imm),
    extension(c).

% JAL x1, offset                            -> c.jal        [C, RV32]
compressed_instruction(c_jal, Offset, 16, Counter) :-
    instruction(jal, Rd, Offset, _, Counter),
    in_class(Rd, x1),
    simm12_lsb0(Offset),
    extension(c), isRV32.

% ADDIW rs1, rs1, imm  (rd == rs1)          -> c.addiw      [C, RV64]
compressed_instruction(c_addiw, Rd, Imm, 16, Counter) :-
    instruction(addiw, Rd, Rd, Imm, _, Counter),
    in_class(Rd, gprnox0),
    simm6(Imm),
    extension(c), isRV64.

% ADDI rd, x0, imm                          -> c.li         [C]
compressed_instruction(c_li, Rd, Imm, 16, Counter) :-
    instruction(addi, Rd, Rs1, Imm, _, Counter),
    in_class(Rd, gprnox0), in_class(Rs1, x0),
    simm6(Imm),
    extension(c).

% ADDIW rd, x0, imm                         -> c.li  (compress-only)  [C, RV64]
compressed_instruction(c_li, Rd, Imm, 16, Counter) :-
    instruction(addiw, Rd, Rs1, Imm, _, Counter),
    in_class(Rd, gprnox0), in_class(Rs1, x0),
    simm6(Imm),
    extension(c), isRV64.

% ADDI x2, x2, imm                          -> c.addi16sp   [C]
compressed_instruction(c_addi16sp, Imm, 16, Counter) :-
    instruction(addi, Rd, Rd, Imm, _, Counter),
    in_class(Rd, sp),
    simm10_lsb0000nonzero(Imm),
    extension(c).

% LUI rd, imm                               -> c.lui        [C]
compressed_instruction(c_lui, Rd, Imm, 16, Counter) :-
    instruction(lui, Rd, Imm, _, Counter),
    in_class(Rd, gprnox0x2),
    clui_imm(Imm),
    extension(c).

% SRLI rs1, rs1, shamt (rd == rs1)          -> c.srli       [C]
compressed_instruction(c_srli, Rd, Imm, 16, Counter) :-
    instruction(srli, Rd, Rd, Imm, _, Counter),
    in_class(Rd, gprc),
    shamt(Imm),
    extension(c).

% SRAI rs1, rs1, shamt (rd == rs1)          -> c.srai       [C]
compressed_instruction(c_srai, Rd, Imm, 16, Counter) :-
    instruction(srai, Rd, Rd, Imm, _, Counter),
    in_class(Rd, gprc),
    shamt(Imm),
    extension(c).

% ANDI rs1, rs1, imm   (rd == rs1)          -> c.andi       [C]
compressed_instruction(c_andi, Rd, Imm, 16, Counter) :-
    instruction(andi, Rd, Rd, Imm, _, Counter),
    in_class(Rd, gprc),
    simm6(Imm),
    extension(c).

% SUB rs1, rs1, rs2    (rd == rs1)          -> c.sub        [C]
compressed_instruction(c_sub, Rd, Rs2, 16, Counter) :-
    instruction(sub, Rd, Rd, Rs2, _, Counter),
    in_class(Rd, gprc), in_class(Rs2, gprc),
    extension(c).

% XOR rs1, rs1, rs2    (rd == rs1)          -> c.xor        [C]
compressed_instruction(c_xor, Rd, Rs2, 16, Counter) :-
    instruction(xor, Rd, Rd, Rs2, _, Counter),
    in_class(Rd, gprc), in_class(Rs2, gprc),
    extension(c).
% XOR rs1, rs2, rs1  (commuted, compress-only)
compressed_instruction(c_xor, Rd, Rs2, 16, Counter) :-
    instruction(xor, Rd, Rs2, Rd, _, Counter),
    in_class(Rd, gprc), in_class(Rs2, gprc),
    extension(c).

% OR rs1, rs1, rs2     (rd == rs1)          -> c.or         [C]
compressed_instruction(c_or, Rd, Rs2, 16, Counter) :-
    instruction(or, Rd, Rd, Rs2, _, Counter),
    in_class(Rd, gprc), in_class(Rs2, gprc),
    extension(c).
% OR rs1, rs2, rs1  (commuted, compress-only)
compressed_instruction(c_or, Rd, Rs2, 16, Counter) :-
    instruction(or, Rd, Rs2, Rd, _, Counter),
    in_class(Rd, gprc), in_class(Rs2, gprc),
    extension(c).

% AND rs1, rs1, rs2    (rd == rs1)          -> c.and        [C]
compressed_instruction(c_and, Rd, Rs2, 16, Counter) :-
    instruction(and, Rd, Rd, Rs2, _, Counter),
    in_class(Rd, gprc), in_class(Rs2, gprc),
    extension(c).
% AND rs1, rs2, rs1  (commuted, compress-only)
compressed_instruction(c_and, Rd, Rs2, 16, Counter) :-
    instruction(and, Rd, Rs2, Rd, _, Counter),
    in_class(Rd, gprc), in_class(Rs2, gprc),
    extension(c).

% SUBW rs1, rs1, rs2   (rd == rs1)          -> c.subw       [C, RV64]
compressed_instruction(c_subw, Rd, Rs2, 16, Counter) :-
    instruction(subw, Rd, Rd, Rs2, _, Counter),
    in_class(Rd, gprc), in_class(Rs2, gprc),
    extension(c), isRV64.

% ADDW rs1, rs1, rs2   (rd == rs1)          -> c.addw       [C, RV64]
compressed_instruction(c_addw, Rd, Rs2, 16, Counter) :-
    instruction(addw, Rd, Rd, Rs2, _, Counter),
    in_class(Rd, gprc), in_class(Rs2, gprc),
    extension(c), isRV64.
% ADDW rs1, rs2, rs1  (commuted, compress-only)
compressed_instruction(c_addw, Rd, Rs2, 16, Counter) :-
    instruction(addw, Rd, Rs2, Rd, _, Counter),
    in_class(Rd, gprc), in_class(Rs2, gprc),
    extension(c), isRV64.

% JAL x0, offset                            -> c.j          [C]
compressed_instruction(c_j, Offset, 16, Counter) :-
    instruction(jal, Rd, Offset, _, Counter),
    in_class(Rd, x0),
    simm12_lsb0(Offset),
    extension(c).

% BEQ rs1, x0, imm                          -> c.beqz       [C]
compressed_instruction(c_beqz, Rs1, Imm, 16, Counter) :-
    instruction(beq, Rs1, Rs2, Imm, _, Counter),
    in_class(Rs1, gprc), in_class(Rs2, x0),
    simm9_lsb0(Imm),
    extension(c).

% BNE rs1, x0, imm                          -> c.bnez       [C]
compressed_instruction(c_bnez, Rs1, Imm, 16, Counter) :-
    instruction(bne, Rs1, Rs2, Imm, _, Counter),
    in_class(Rs1, gprc), in_class(Rs2, x0),
    simm9_lsb0(Imm),
    extension(c).


%===========================================================================%
% Quadrant 2                                                                %
%===========================================================================%

% SLLI rs1, rs1, shamt (rd == rs1)          -> c.slli       [C]
compressed_instruction(c_slli, Rd, Imm, 16, Counter) :-
    instruction(slli, Rd, Rd, Imm, _, Counter),
    in_class(Rd, gprnox0),
    shamt(Imm),
    extension(c).

% FLD rd, sp, imm                           -> c.fldsp      [C, D]
compressed_instruction(c_fldsp, Rd, Rs1, Imm, 16, Counter) :-
    instruction(fld, Rd, Rs1, Imm, _, Counter),
    in_class(Rd, fpr64), in_class(Rs1, sp),
    uimm9_lsb000(Imm),
    extension(c), extension(d).

% LW rd, sp, imm                            -> c.lwsp       [C]
compressed_instruction(c_lwsp, Rd, Rs1, Imm, 16, Counter) :-
    instruction(lw, Rd, Rs1, Imm, _, Counter),
    in_class(Rd, gprnox0), in_class(Rs1, sp),
    uimm8_lsb00(Imm),
    extension(c).

% FLW rd, sp, imm                           -> c.flwsp      [C, F, RV32]
compressed_instruction(c_flwsp, Rd, Rs1, Imm, 16, Counter) :-
    instruction(flw, Rd, Rs1, Imm, _, Counter),
    in_class(Rd, fpr32), in_class(Rs1, sp),
    uimm8_lsb00(Imm),
    extension(c), extension(f), isRV32.

% LD rd, sp, imm                            -> c.ldsp       [C, RV64]
compressed_instruction(c_ldsp, Rd, Rs1, Imm, 16, Counter) :-
    instruction(ld, Rd, Rs1, Imm, _, Counter),
    in_class(Rd, gprnox0), in_class(Rs1, sp),
    uimm9_lsb000(Imm),
    extension(c), isRV64.

% JALR x0, rs1, 0                           -> c.jr         [C]
compressed_instruction(c_jr, Rs1, 16, Counter) :-
    instruction(jalr, Rd, Rs1, Imm, _, Counter),
    in_class(Rd, x0), in_class(Rs1, gprnox0),
    zero(Imm),
    extension(c).

% ADD rs1, x0, rs2                          -> c.mv (compress-only)  [C]
compressed_instruction(c_mv, Rd, Rs2, 16, Counter) :-
    instruction(add, Rd, Rz, Rs2, _, Counter),
    in_class(Rd, gprnox0), in_class(Rz, x0), in_class(Rs2, gprnox0),
    extension(c).
% ADD rs1, rs2, x0  (commuted, compress-only)
compressed_instruction(c_mv, Rd, Rs2, 16, Counter) :-
    instruction(add, Rd, Rs2, Rz, _, Counter),
    in_class(Rd, gprnox0), in_class(Rs2, gprnox0), in_class(Rz, x0),
    extension(c).
% ADDI rs1, rs2, 0                          -> c.mv         [C]
compressed_instruction(c_mv, Rd, Rs2, 16, Counter) :-
    instruction(addi, Rd, Rs2, Imm, _, Counter),
    in_class(Rd, gprnox0), in_class(Rs2, gprnox0),
    zero(Imm),
    extension(c).

% EBREAK                                    -> c.ebreak     [C]
compressed_instruction(c_ebreak, 16, Counter) :-
    instruction(ebreak, _, Counter),
    extension(c).

% UNIMP                                     -> c.unimp      [C]
compressed_instruction(c_unimp, 16, Counter) :-
    instruction(unimp, _, Counter),
    extension(c).

% JALR x1, rs1, 0                           -> c.jalr       [C]
compressed_instruction(c_jalr, Rs1, 16, Counter) :-
    instruction(jalr, Rd, Rs1, Imm, _, Counter),
    in_class(Rd, x1), in_class(Rs1, gprnox0),
    zero(Imm),
    extension(c).

% ADD rs1, rs1, rs2    (rd == rs1)          -> c.add        [C]
compressed_instruction(c_add, Rd, Rs2, 16, Counter) :-
    instruction(add, Rd, Rd, Rs2, _, Counter),
    in_class(Rd, gprnox0), in_class(Rs2, gprnox0),
    extension(c).
% ADD rs1, rs2, rs1  (commuted, compress-only)
compressed_instruction(c_add, Rd, Rs2, 16, Counter) :-
    instruction(add, Rd, Rs2, Rd, _, Counter),
    in_class(Rd, gprnox0), in_class(Rs2, gprnox0),
    extension(c).

% FSD rs2, sp, imm                          -> c.fsdsp      [C, D]
compressed_instruction(c_fsdsp, Rs2, Rs1, Imm, 16, Counter) :-
    instruction(fsd, Rs2, Rs1, Imm, _, Counter),
    in_class(Rs2, fpr64), in_class(Rs1, sp),
    uimm9_lsb000(Imm),
    extension(c), extension(d).

% SW rs2, sp, imm                           -> c.swsp       [C]
compressed_instruction(c_swsp, Rs2, Rs1, Imm, 16, Counter) :-
    instruction(sw, Rs2, Rs1, Imm, _, Counter),
    in_class(Rs2, gpr), in_class(Rs1, sp),
    uimm8_lsb00(Imm),
    extension(c).

% FSW rs2, sp, imm                          -> c.fswsp      [C, F, RV32]
compressed_instruction(c_fswsp, Rs2, Rs1, Imm, 16, Counter) :-
    instruction(fsw, Rs2, Rs1, Imm, _, Counter),
    in_class(Rs2, fpr32), in_class(Rs1, sp),
    uimm8_lsb00(Imm),
    extension(c), extension(f), isRV32.

% SD rs2, sp, imm                           -> c.sdsp       [C, RV64]
compressed_instruction(c_sdsp, Rs2, Rs1, Imm, 16, Counter) :-
    instruction(sd, Rs2, Rs1, Imm, _, Counter),
    in_class(Rs2, gpr), in_class(Rs1, sp),
    uimm9_lsb000(Imm),
    extension(c), isRV64.
