/* libdt3.so: lives in /opt/dtlib, outside the default search path; found
 * through LD_LIBRARY_PATH (dt-ldlp) or DT_RUNPATH $ORIGIN (dt-rpath). */
int dt3_value = 333;
int dt3_fn(int x) { return x * 3; }
