/* dt-ldlp / dt-rpath: linked against libdt3.so, which is in /opt/dtlib.
 * dt-ldlp needs LD_LIBRARY_PATH; dt-rpath has DT_RUNPATH $ORIGIN/../opt/dtlib. */
#include <stdio.h>
#include <string.h>

extern int dt3_value;
int dt3_fn(int);

int main(int argc, char **argv)
{
	int ok = dt3_value == 333 && dt3_fn(14) == 42;

	printf("%s %s: libdt3 dt3_value=%d dt3_fn(14)=%d\n", ok ? "PASS" : "FAIL",
	       argc > 1 ? argv[1] : "ldlp-child", dt3_value, dt3_fn(14));
	return ok ? 0 : 1;
}
