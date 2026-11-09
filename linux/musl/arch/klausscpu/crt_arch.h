/* argc is at [sp]; START_c(sp) never returns. */
__asm__(
".text\n"
".global " START "\n"
".type " START ",%function\n"
START ":\n"
"	getsp r0\n"
"	andi r1, r0, -16\n"
"	setsp r1\n"
"	setr r15, 0\n"
"	call " START "_c\n"
"1:	jmp 1b\n"
);
