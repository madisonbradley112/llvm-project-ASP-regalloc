; High GPRC-pressure: many simultaneously-live values each feeding a
; two-address compressible op (c.and/c.or/c.xor). More live compressible
; operands than the 8 GPRC regs, so the allocator must choose which subset
; to land in x8-x15. Local greedy can pick a suboptimal subset; the ASP
; phase-1 model maximizes the realized compressions globally.
target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128"
target triple = "riscv64-unknown-elf"

define i64 @pressure(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e, i64 %f, i64 %g, i64 %h) #0 {
entry:
  ; Build 10 independent values that all stay live to the end.
  %v0 = mul i64 %a, %b
  %v1 = mul i64 %b, %c
  %v2 = mul i64 %c, %d
  %v3 = mul i64 %d, %e
  %v4 = mul i64 %e, %f
  %v5 = mul i64 %f, %g
  %v6 = mul i64 %g, %h
  %v7 = mul i64 %h, %a
  %v8 = mul i64 %a, %c
  %v9 = mul i64 %b, %d

  ; Two-address compressible chain: each op's first source dies here, so
  ; rd can coalesce with rs1 -> c.and/c.or/c.xor if it lands in GPRC.
  %w0 = and i64 %v0, %v1
  %w1 = or  i64 %w0, %v2
  %w2 = xor i64 %w1, %v3
  %w3 = and i64 %w2, %v4
  %w4 = or  i64 %w3, %v5
  %w5 = xor i64 %w4, %v6
  %w6 = and i64 %w5, %v7
  %w7 = or  i64 %w6, %v8
  %w8 = xor i64 %w7, %v9
  ret i64 %w8
}

attributes #0 = { "target-features"="+c,+m" }
