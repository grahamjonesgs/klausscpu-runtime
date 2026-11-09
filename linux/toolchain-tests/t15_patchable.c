/* -fpatchable-function-entry=2 (ftrace) */
__attribute__((patchable_function_entry(2))) int f(int x) { return x + 1; }
