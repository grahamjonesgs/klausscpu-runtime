/* "i"(&sym + off) */
extern int arr[];
void f(void) { asm volatile(".pushsection .data.t07,\"aw\"\n\t.quad %c0\n\t.popsection" :: "i"(&arr[3])); }
