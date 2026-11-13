#include "eh_lib.h"

#include <stdexcept>

#define NOINLINE __attribute__((noinline))

eh_lib_error::~eh_lib_error() {}

void eh_lib_throw(int code) { throw eh_lib_error(code); }

int eh_lib_catch(void (*f)()) {
  try {
    f();
  } catch (const std::out_of_range &) {
    return 1;
  }
  return 0;
}

namespace {
struct Guard {
  int *flag;
  ~Guard() { *flag = 1; }
};
// A cleanup landing pad in the library: the destructor runs, then
// _Unwind_Resume carries the exception on to eh_lib_cleanup.
NOINLINE void inner(void (*f)(), int *flag) {
  Guard g{flag};
  f();
}
} // namespace

int eh_lib_cleanup(void (*f)()) {
  int cleaned = 0;
  try {
    inner(f, &cleaned);
  } catch (...) {
    return cleaned;
  }
  return 0;
}
