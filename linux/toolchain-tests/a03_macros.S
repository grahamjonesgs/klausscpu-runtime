/* .macro with defaults/varargs, .irp, .rept, .altmacro + LOCAL, .if/.ifdef/.ifb, \@ */
	.macro SAVE regs:vararg
	.irp r, \regs
	push \r
	.endr
	.endm
	.macro ADDN reg, n=1
	addi \reg, \reg, \n
	.endm
	.macro UNIQ
	.Luniq\@:
	nop
	.endm
	.altmacro
	.macro ALT x
	LOCAL lab
lab:	nop
	.endm
	.noaltmacro
	.text
	SAVE r4, r5, r6
	ADDN r1
	ADDN r1, 8
	UNIQ
	UNIQ
	.rept 3
	nop
	.endr
	.ifdef FOO
	nop
	.else
	nop
	.endif
	.if 1 == 1
	nop
	.endif
	.ifb
	nop
	.endif
	.set SYMV, 0x40
	.equ SYMW, SYMV + 8
	setr r1, SYMW
