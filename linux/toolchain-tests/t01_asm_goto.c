/* asm goto without outputs (static keys / jump labels) */
int f(int x) {
    asm goto("fbri %0, 0, 8, %l[yes]" :: "r"(x) :: yes);
    return 0;
yes:
    return 1;
}
