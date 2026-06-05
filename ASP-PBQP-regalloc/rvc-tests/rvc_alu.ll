; Two-address-friendly ALU chain: each op's first source dies, so rd can
; coalesce with rs1 -> c.and/c.or/c.xor/c.sub if operands land in GPRC.
target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128"
target triple = "riscv64-unknown-elf"

define i64 @chain(i64 %a, i64 %b, i64 %c, i64 %d) #0 {
entry:
  %t0 = and i64 %a, %b
  %t1 = or  i64 %t0, %c
  %t2 = xor i64 %t1, %d
  %t3 = sub i64 %t2, %a
  %t4 = and i64 %t3, %b
  %t5 = or  i64 %t4, %c
  ret i64 %t5
}

attributes #0 = { "target-features"="+c,+m" }
