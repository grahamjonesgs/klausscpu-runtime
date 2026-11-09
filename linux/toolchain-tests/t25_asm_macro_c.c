/* .macro / .irp / .rept / .altmacro inside inline asm */
asm(".macro PUSHALL\n.irp r,r4,r5,r6,r7\n\tpush \\r\n.endr\n.endm\n");
void f(void) { asm volatile("PUSHALL\n\t.rept 2\n\tnop\n\t.endr"); }
