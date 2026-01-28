	.build_version macos, 15, 0
	.section	__TEXT,__text,regular,pure_instructions
	.globl	_foo                            ; -- Begin function foo
	.p2align	2
_foo:                                   ; @foo
	.cfi_startproc
; %bb.0:                                ; %entry
	sub	sp, sp, #160
	stp	x28, x27, [sp, #64]             ; 16-byte Folded Spill
	stp	x26, x25, [sp, #80]             ; 16-byte Folded Spill
	stp	x24, x23, [sp, #96]             ; 16-byte Folded Spill
	stp	x22, x21, [sp, #112]            ; 16-byte Folded Spill
	stp	x20, x19, [sp, #128]            ; 16-byte Folded Spill
	stp	x29, x30, [sp, #144]            ; 16-byte Folded Spill
	.cfi_def_cfa_offset 160
	.cfi_offset w30, -8
	.cfi_offset w29, -16
	.cfi_offset w19, -24
	.cfi_offset w20, -32
	.cfi_offset w21, -40
	.cfi_offset w22, -48
	.cfi_offset w23, -56
	.cfi_offset w24, -64
	.cfi_offset w25, -72
	.cfi_offset w26, -80
	.cfi_offset w27, -88
	.cfi_offset w28, -96
	bl	_baz
	str	w0, [sp, #60]                   ; 4-byte Folded Spill
	bl	_baz
	str	w0, [sp, #56]                   ; 4-byte Folded Spill
	bl	_baz
	str	w0, [sp, #52]                   ; 4-byte Folded Spill
	bl	_baz
	str	w0, [sp, #48]                   ; 4-byte Folded Spill
	bl	_baz
	str	w0, [sp, #44]                   ; 4-byte Folded Spill
	bl	_baz
	str	w0, [sp, #40]                   ; 4-byte Folded Spill
	bl	_baz
	mov	w28, w0
	bl	_baz
	mov	w27, w0
	bl	_baz
	mov	w26, w0
	bl	_baz
	mov	w25, w0
	bl	_baz
	mov	w24, w0
	bl	_baz
	mov	w23, w0
	bl	_baz
	mov	w22, w0
	bl	_baz
	mov	w21, w0
	bl	_baz
	mov	w20, w0
	bl	_baz
	mov	w19, w0
	bl	_baz
	stp	w19, w0, [sp, #28]
	mov	w6, w28
	mov	w7, w27
	ldp	w1, w0, [sp, #56]               ; 8-byte Folded Reload
	ldp	w3, w2, [sp, #48]               ; 8-byte Folded Reload
	ldp	w5, w4, [sp, #40]               ; 8-byte Folded Reload
	stp	w21, w20, [sp, #20]
	stp	w23, w22, [sp, #12]
	stp	w25, w24, [sp, #4]
	str	w26, [sp]
	bl	_bar
	ldp	x29, x30, [sp, #144]            ; 16-byte Folded Reload
	ldp	x20, x19, [sp, #128]            ; 16-byte Folded Reload
	ldp	x22, x21, [sp, #112]            ; 16-byte Folded Reload
	ldp	x24, x23, [sp, #96]             ; 16-byte Folded Reload
	ldp	x26, x25, [sp, #80]             ; 16-byte Folded Reload
	ldp	x28, x27, [sp, #64]             ; 16-byte Folded Reload
	add	sp, sp, #160
	ret
	.cfi_endproc
                                        ; -- End function
.subsections_via_symbols
