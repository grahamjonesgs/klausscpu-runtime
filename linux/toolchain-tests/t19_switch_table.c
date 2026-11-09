/* jump tables (needs relocations into .rodata) */
int f(int x) { switch (x) { case 0: return 11; case 1: return 22; case 2: return 33; case 3: return 44; case 4: return 55; case 5: return 66; case 6: return 77; default: return 0; } }
