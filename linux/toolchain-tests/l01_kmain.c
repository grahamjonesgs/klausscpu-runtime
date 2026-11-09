extern char _end[], __bss_start[];
long counter;
const char msg[] = "hi";
void kmain(void) { counter = _end - __bss_start + msg[0]; }
