/* top-level asm with symbol references, .globl, .type, .size, numeric labels */
asm(".text\n.globl tl\n.type tl, @function\ntl:\n1:\n\tjmp 1b\n.size tl, .-tl\n");
