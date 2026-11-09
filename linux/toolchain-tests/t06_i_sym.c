/* "i"(&sym) - address of a symbol as an immediate (jump labels, static calls) */
extern int some_key;
void f(void) { asm volatile(".pushsection __jump_table,\"aw\"\n\t.quad %c0\n\t.popsection" :: "i"(&some_key)); }
