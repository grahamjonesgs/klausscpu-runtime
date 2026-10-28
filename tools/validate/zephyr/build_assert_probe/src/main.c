/* Must fail to compile; see ../CMakeLists.txt. */
#include <zephyr/toolchain.h>

BUILD_ASSERT(0, "KLAUSSCPU_BUILD_ASSERT_PROBE");

int main(void)
{
	return 0;
}
