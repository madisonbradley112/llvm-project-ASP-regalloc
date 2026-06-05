; Loads/stores through a common base pointer: c.lw/c.sw want base+data in GPRC.
target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128"
target triple = "riscv64-unknown-elf"

define void @copy4(ptr %p, ptr %q) #0 {
entry:
  %a0 = getelementptr inbounds i32, ptr %p, i64 0
  %a1 = getelementptr inbounds i32, ptr %p, i64 1
  %a2 = getelementptr inbounds i32, ptr %p, i64 2
  %a3 = getelementptr inbounds i32, ptr %p, i64 3
  %v0 = load i32, ptr %a0
  %v1 = load i32, ptr %a1
  %v2 = load i32, ptr %a2
  %v3 = load i32, ptr %a3
  %b0 = getelementptr inbounds i32, ptr %q, i64 0
  %b1 = getelementptr inbounds i32, ptr %q, i64 1
  %b2 = getelementptr inbounds i32, ptr %q, i64 2
  %b3 = getelementptr inbounds i32, ptr %q, i64 3
  store i32 %v0, ptr %b0
  store i32 %v1, ptr %b1
  store i32 %v2, ptr %b2
  store i32 %v3, ptr %b3
  ret void
}

attributes #0 = { "target-features"="+c,+m" }
