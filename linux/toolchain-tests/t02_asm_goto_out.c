/* asm goto with outputs (CONFIG_CC_HAS_ASM_GOTO_OUTPUT, uaccess) */
long f(long *p) {
    long v;
    asm goto("ldidx64 %0, %1, 0\n\tjmp %l[err]" : "=r"(v) : "r"(p) :: err);
    return v;
err:
    return -14;
}
