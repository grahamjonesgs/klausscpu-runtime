/* -fstack-protector-strong */
void g(char *);
int f(void) { char buf[64]; g(buf); return buf[0]; }
