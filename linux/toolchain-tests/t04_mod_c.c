/* %c modifier: a bare constant (used by jump labels, alternatives, bug tables) */
void f(void) { asm volatile(".pushsection .discard.t04,\"a\"\n\t.quad %c0\n\t.popsection" :: "i"(42)); }
