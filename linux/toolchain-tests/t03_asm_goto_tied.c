/* asm goto with an in/out ("+r") operand */
long f(long v) {
    asm goto("addi %0, %0, 1\n\tjmp %l[out]" : "+r"(v) :: : out);
    return v;
out:
    return v + 100;
}
