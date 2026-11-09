/* weak, alias, section, used, aligned, noinline, cold, visibility */
int __attribute__((weak)) w(void) { return 1; }
int real(void) { return 2; }
int al(void) __attribute__((alias("real")));
static int __attribute__((section(".init.text"), used, noinline, cold)) ini(void) { return 3; }
int __attribute__((aligned(64))) big[16];
__attribute__((visibility("hidden"))) int hid(void) { return 4; }
int (*keep)(void) = ini;
