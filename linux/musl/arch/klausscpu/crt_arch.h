/* argc is at [sp]; START_c(sp) never returns.  The dynamic linker's
 * _dlstart (and rcrt1's _start), built with SHARED, also passes the
 * address of its own _DYNAMIC (an array of Elf32_Dyn): a hidden symbol,
 * so leapc reaches it PC-relative before anything is relocated. */
__asm__(
".text\n"
".global " START "\n"
".type " START ",%function\n"
START ":\n"
"	getsp r0\n"
"	andi r1, r0, -16\n"
"	setsp r1\n"
"	setr r15, 0\n"
#ifdef SHARED
".weak _DYNAMIC\n"
".hidden _DYNAMIC\n"
"	leapc r1, _DYNAMIC\n"
#endif
"	call " START "_c\n"
"1:	jmp 1b\n"
);
