; Divergence hunt: 2 GPRC base pointers (p=a0/x10, q=a1/x11). Load 10 values
; from p, keep them ALL live, then store each to q. Each loaded value placed in
; GPRC wins BOTH its load (c.lw, base p in GPRC) and its store (c.sw, base q in
; GPRC) -> two compressions per value. There are more live values (10) than
; free caller-saved GPRC regs, so only a subset can win; the choice of which is
; a global packing problem. In-tree LLVM emits NO hints for loads/stores, so the
; baseline's compression here is incidental to greedy's register choices.
target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128"
target triple = "riscv64-unknown-elf"

define void @gatherstore(ptr %p, ptr %q) #0 {
entry:
  %g0 = getelementptr inbounds i32, ptr %p, i64 0
  %g1 = getelementptr inbounds i32, ptr %p, i64 1
  %g2 = getelementptr inbounds i32, ptr %p, i64 2
  %g3 = getelementptr inbounds i32, ptr %p, i64 3
  %g4 = getelementptr inbounds i32, ptr %p, i64 4
  %g5 = getelementptr inbounds i32, ptr %p, i64 5
  %g6 = getelementptr inbounds i32, ptr %p, i64 6
  %g7 = getelementptr inbounds i32, ptr %p, i64 7
  %g8 = getelementptr inbounds i32, ptr %p, i64 8
  %g9 = getelementptr inbounds i32, ptr %p, i64 9
  %v0 = load i32, ptr %g0
  %v1 = load i32, ptr %g1
  %v2 = load i32, ptr %g2
  %v3 = load i32, ptr %g3
  %v4 = load i32, ptr %g4
  %v5 = load i32, ptr %g5
  %v6 = load i32, ptr %g6
  %v7 = load i32, ptr %g7
  %v8 = load i32, ptr %g8
  %v9 = load i32, ptr %g9
  ; All ten values live simultaneously across this point.
  %s0 = getelementptr inbounds i32, ptr %q, i64 0
  %s1 = getelementptr inbounds i32, ptr %q, i64 1
  %s2 = getelementptr inbounds i32, ptr %q, i64 2
  %s3 = getelementptr inbounds i32, ptr %q, i64 3
  %s4 = getelementptr inbounds i32, ptr %q, i64 4
  %s5 = getelementptr inbounds i32, ptr %q, i64 5
  %s6 = getelementptr inbounds i32, ptr %q, i64 6
  %s7 = getelementptr inbounds i32, ptr %q, i64 7
  %s8 = getelementptr inbounds i32, ptr %q, i64 8
  %s9 = getelementptr inbounds i32, ptr %q, i64 9
  store i32 %v0, ptr %s0
  store i32 %v1, ptr %s1
  store i32 %v2, ptr %s2
  store i32 %v3, ptr %s3
  store i32 %v4, ptr %s4
  store i32 %v5, ptr %s5
  store i32 %v6, ptr %s6
  store i32 %v7, ptr %s7
  store i32 %v8, ptr %s8
  store i32 %v9, ptr %s9
  ret void
}

attributes #0 = { "target-features"="+c,+m" }
