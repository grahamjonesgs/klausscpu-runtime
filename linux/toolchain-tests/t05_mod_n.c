/* %n modifier: negated constant */
void f(void) { asm volatile(".pushsection .discard.t05,\"a\"\n\t.quad %n0\n\t.popsection" :: "i"(42)); }
