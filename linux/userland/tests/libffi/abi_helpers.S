# abi_helpers.S: assembly helpers for abi.c, which see the registers and
# stack slots exactly as the KlaussCPU ABI places them (klausscpu-llvm
# ABI.md), without a compiler in between.

	.text

# kl_capture: any signature.  Stores R0-R3 in kl_cap[0..3] and the 12
# doublewords from the entry SP + 8 (the stack arguments of a
# non-variadic call; the home area and then the stack arguments of a
# variadic one) in kl_cap[4..15], and returns kl_ret[0] in R12 and
# kl_ret[1] in R11.
	.globl	kl_capture
	.type	kl_capture, @function
kl_capture:
	leapc	r8, kl_cap
	stidx64	r0, r8, 0
	stidx64	r1, r8, 8
	stidx64	r2, r8, 16
	stidx64	r3, r8, 24
	getsp	r9
	setr	r10, 12
1:	ldidx64	r11, r9, 8
	stidx64	r11, r8, 32
	addi	r9, r9, 8
	addi	r8, r8, 8
	addi	r10, r10, -1
	fbri	r10, 0, 8, 1b		# NE
	leapc	r8, kl_ret
	ldidx64	r12, r8, 0
	ldidx64	r11, r8, 8
	ret
	.size	kl_capture, . - kl_capture

# void kl_call_raw(struct kl_raw *c): calls c->fn with R0-R3 = c->r[0..3]
# (no stack arguments) and stores R12 and R11 after it in c->out[0..1].
# struct kl_raw { void *fn; uint64_t r[4]; uint64_t out[2]; }
	.globl	kl_call_raw
	.type	kl_call_raw, @function
kl_call_raw:
	enter	0
	push	r4
	copy	r4, r0
	ldidx64	r0, r4, 8
	ldidx64	r1, r4, 16
	ldidx64	r2, r4, 24
	ldidx64	r3, r4, 32
	ldidx64	r13, r4, 0
	callr	r13
	stidx64	r12, r4, 40
	stidx64	r11, r4, 48
	ldidx64	r4, r15, -8
	leaveret
	.size	kl_call_raw, . - kl_call_raw

	.section .note.GNU-stack,"",%progbits
