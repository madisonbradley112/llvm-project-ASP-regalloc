; Load/store pressure: many distinct base pointers, each loaded once, all
; results live simultaneously, then scattered through a separate base. c.lw
; needs base+dest in GPRC; c.sw needs base+data in GPRC. There is NO
; two-address tie and NO coalescing chain here, so this isolates the pure
; GPRC-placement decision. In-tree LLVM does not hint loads/stores toward
; GPRC at all (see RISCVRegisterInfo isCompressible), so any compression in
; the baseline is incidental; the ASP pass targets it directly.
target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128"
target triple = "riscv64-unknown-elf"

define void @gather_scatter(ptr %p0, ptr %p1, ptr %p2, ptr %p3, ptr %p4, ptr %p5, ptr %q) #0 {
entry:
  %v0 = load i32, ptr %p0
  %v1 = load i32, ptr %p1
  %v2 = load i32, ptr %p2
  %v3 = load i32, ptr %p3
  %v4 = load i32, ptr %p4
  %v5 = load i32, ptr %p5
  ; Keep all six values live to the scatter below.
  %s0 = getelementptr inbounds i32, ptr %q, i64 0
  %s1 = getelementptr inbounds i32, ptr %q, i64 1
  %s2 = getelementptr inbounds i32, ptr %q, i64 2
  %s3 = getelementptr inbounds i32, ptr %q, i64 3
  %s4 = getelementptr inbounds i32, ptr %q, i64 4
  %s5 = getelementptr inbounds i32, ptr %q, i64 5
  store i32 %v0, ptr %s0
  store i32 %v1, ptr %s1
  store i32 %v2, ptr %s2
  store i32 %v3, ptr %s3
  store i32 %v4, ptr %s4
  store i32 %v5, ptr %s5
  ret void
}

attributes #0 = { "target-features"="+c,+m" }
