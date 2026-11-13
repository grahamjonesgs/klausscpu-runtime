// std::terminate paths (run.sh checks the exit status and output):
//   terminate noexcept   an exception leaving a noexcept function calls the
//                        terminate handler, which exits with 77
//   terminate uncaught   an uncaught exception: libc++abi's message, SIGABRT
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>

__attribute__((noinline)) void thrower() { throw std::runtime_error("escaped"); }
__attribute__((noinline)) void nothrow_wrapper() noexcept { thrower(); }

int main(int argc, char **argv) {
  if (argc > 1 && std::strcmp(argv[1], "noexcept") == 0) {
    std::set_terminate([] {
      std::printf("TERMINATE-HANDLER\n");
      std::fflush(stdout);
      std::_Exit(77);
    });
    nothrow_wrapper();
    std::printf("FAIL returned from noexcept\n");
    return 1;
  }
  thrower();
  return 1;
}
