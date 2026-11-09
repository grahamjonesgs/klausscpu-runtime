/* labels as values (BPF interpreter) */
int f(int i) { static void *t[] = { &&a, &&b }; goto *t[i & 1]; a: return 1; b: return 2; }
