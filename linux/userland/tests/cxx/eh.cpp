// C++ exceptions on KlaussCPU Linux (CLANG_ON_BOARD_PLAN.md M2): libunwind's
// KlaussCPU port, libc++abi, and the backend's landing pads and CFI.
// Prints PASS/FAIL lines and "DONE eh failures=N"; exit status = failures.
// Built by run.sh at several optimisation levels, static and as a PIE with
// the shared libraries, and with -DEH_LIB linked against libehlib.so
// (eh_lib.cpp), whose functions throw into and catch from this program.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <unwind.h>
#include <vector>

#include "eh_lib.h"

#define NOINLINE __attribute__((noinline))

static int failures;
static void check(bool ok, const char *name, int line) {
  if (ok)
    std::printf("PASS %s\n", name);
  else {
    std::printf("FAIL %s (line %d)\n", name, line);
    ++failures;
  }
}
#define CHECK(c, name) check((c), (name), __LINE__)

// Opaque values, so that the optimiser cannot fold the tests away.
volatile long vzero = 0, vone = 1, vseven = 7;

static int dtors;
struct Counted {
  int id;
  explicit Counted(int i) : id(i) {}
  ~Counted() { ++dtors; }
};

NOINLINE void throw_int(int v) { throw v; }

NOINLINE int depth(int n) {
  Counted c(n);
  if (n == 0)
    throw std::runtime_error("deep");
  return depth(n - 1) + c.id;
}

// Uses the callee-saved registers R4-R7 (and everything else) before it
// throws, so the unwinder must restore its callers' values from this frame's
// CFI.
NOINLINE void clobber_and_throw(long seed) {
  long a = seed * 3, b = seed * 5, c = seed * 7, d = seed * 11, e = seed * 13,
       f = seed * 17, g = seed * 19;
  for (int i = 0; i < vseven; ++i) {
    a += b ^ c;
    b += c ^ d;
    c += d ^ e;
    d += e ^ f;
    e += f ^ g;
    f += g ^ a;
    g += a ^ b;
    throw_int(a == 12345 ? 1 : 0); // always throws
  }
  std::printf("unreachable %ld\n", a + b + c + d + e + f + g);
}

NOINLINE long callee_saved(long x) {
  // Live across the call: in callee-saved registers at -O1 and up.
  long a = x * 2 + vone, b = x * 3 + vone, c = x * 5 + vone, d = x * 7 + vone,
       e = x * 9 + vone;
  try {
    clobber_and_throw(x);
  } catch (int) {
  }
  return a + b * 10 + c * 100 + d * 1000 + e * 10000;
}

struct Base : std::exception {
  const char *what() const noexcept override { return "Base"; }
};
struct Derived : Base {
  int v;
  explicit Derived(int x) : v(x) {}
  const char *what() const noexcept override { return "Derived"; }
};

static int built, destroyed;
struct Fragile {
  Fragile() {
    if (built == 3)
      throw std::length_error("fourth");
    ++built;
  }
  ~Fragile() { ++destroyed; }
};

static int uncaught_in_dtor = -1;
struct Probe {
  ~Probe() { uncaught_in_dtor = std::uncaught_exceptions(); }
};

// A dynamic alloca makes the frame keep its call frames out of the prologue
// (ADDSP around each call): a call with stack arguments that throws is
// resumed in its landing pad through DW_CFA_GNU_args_size.  Catching in a
// loop would leak the 40 bytes each time without it.
NOINLINE void args9(long a, long b, long c, long d, long e, long f, long g,
                    long h, long i) {
  if (a + b + c + d + e + f + g + h + i == 36 + vzero)
    throw 9;
}
NOINLINE uintptr_t sp_probe() {
  volatile char c = 0;
  return reinterpret_cast<uintptr_t>(&c);
}
NOINLINE int vla_loop(long n) {
  char *buf = static_cast<char *>(__builtin_alloca(n));
  std::memset(buf, 1, n);
  uintptr_t before = sp_probe();
  int caught = 0;
  for (int i = 0; i < 2000; ++i) {
    try {
      args9(0, 1, 2, 3, 4, 5, 6, 7, 8);
    } catch (int) {
      ++caught;
    }
  }
  uintptr_t after = sp_probe();
  return (before == after && buf[n - 1] == 1) ? caught : -1;
}

// An over-aligned local realigns the frame (base pointer R7).
NOINLINE int aligned_frame(int x) {
  alignas(64) char big[64];
  std::memset(big, x, sizeof big);
  try {
    throw_int(x);
  } catch (int v) {
    return v + big[63] + (reinterpret_cast<uintptr_t>(big) % 64 == 0 ? 0 : 1000);
  }
  return -1;
}

// Shrink-wrapping candidate: no frame on the early-exit path.
NOINLINE int maybe_throw(int x) {
  if (x < 0)
    return x;
  try {
    throw_int(x + 1);
  } catch (int v) {
    return v;
  }
  return 0;
}

static _Unwind_Reason_Code bt_cb(struct _Unwind_Context *ctx, void *arg) {
  ++*static_cast<int *>(arg);
  return _Unwind_GetIP(ctx) ? _URC_NO_REASON : _URC_END_OF_STACK;
}
NOINLINE int backtrace_depth(int n) {
  if (n > 0)
    return backtrace_depth(n - 1) + int(vzero);
  int frames = 0;
  _Unwind_Backtrace(bt_cb, &frames);
  return frames;
}

NOINLINE void callback_thrower() { throw std::out_of_range("from main"); }

int main() {
  // Throw and catch in one function.
  try {
    throw 1;
  } catch (int v) {
    CHECK(v == 1, "local-throw");
  }

  // Across frames, with destructors run during unwinding.
  dtors = 0;
  try {
    depth(10);
    CHECK(false, "frames-throw");
  } catch (const std::runtime_error &e) {
    CHECK(std::strcmp(e.what(), "deep") == 0, "frames-what");
  }
  CHECK(dtors == 11, "frames-dtors");

  // Rethrow.
  try {
    try {
      throw_int(5);
    } catch (int) {
      throw;
    }
  } catch (int v) {
    CHECK(v == 5, "rethrow");
  }

  // catch (...), and a new exception from inside a handler.
  bool any = false;
  try {
    throw 2.5;
  } catch (...) {
    any = true;
  }
  CHECK(any, "catch-all");
  try {
    try {
      throw 1;
    } catch (int) {
      throw std::string("second");
    }
  } catch (const std::string &s) {
    CHECK(s == "second", "throw-in-handler");
  }

  // Derived caught by base reference; by pointer.
  try {
    throw Derived(7);
  } catch (const Base &b) {
    CHECK(std::strcmp(b.what(), "Derived") == 0 &&
              dynamic_cast<const Derived &>(b).v == 7,
          "catch-by-base");
  }
  static Derived pd(8);
  try {
    throw &pd;
  } catch (Base *b) {
    CHECK(b == &pd, "catch-pointer");
  }

  // std::exception_ptr.
  std::exception_ptr p = std::make_exception_ptr(std::logic_error("lp"));
  try {
    std::rethrow_exception(p);
  } catch (const std::logic_error &e) {
    CHECK(std::strcmp(e.what(), "lp") == 0, "exception-ptr");
  }
  std::exception_ptr cur;
  try {
    throw_int(11);
  } catch (...) {
    cur = std::current_exception();
  }
  try {
    std::rethrow_exception(cur);
  } catch (int v) {
    CHECK(v == 11, "current-exception");
  }

  // The unwinder restores callee-saved registers from the frames it removes.
  long want = 0;
  {
    long x = 3;
    want = (x * 2 + 1) + (x * 3 + 1) * 10 + (x * 5 + 1) * 100 +
           (x * 7 + 1) * 1000 + (x * 9 + 1) * 10000;
  }
  CHECK(callee_saved(3) == want, "callee-saved-restored");

  // A constructor that throws inside new[]: the elements built are destroyed.
  built = destroyed = 0;
  try {
    Fragile *f = new Fragile[6];
    delete[] f;
  } catch (const std::length_error &) {
    CHECK(built == 3 && destroyed == 3, "array-ctor-throw");
  }

  // std::uncaught_exceptions() in a destructor run by the unwinder.
  try {
    Probe pr;
    throw_int(1);
  } catch (int) {
  }
  CHECK(uncaught_in_dtor == 1, "uncaught-exceptions");

  // Frame shapes.
  CHECK(vla_loop(100 + vzero) == 2000, "gnu-args-size-vla");
  CHECK(aligned_frame(4) == 8, "realigned-frame");
  CHECK(maybe_throw(4) == 5 && maybe_throw(-3) == -3, "shrink-wrap");

  // Exceptions in threads, and an exception_ptr carried between threads.
  bool thread_ok = false;
  std::exception_ptr from_thread;
  std::thread t([&] {
    try {
      depth(5);
    } catch (const std::runtime_error &) {
      thread_ok = true;
    }
    try {
      throw Derived(3);
    } catch (...) {
      from_thread = std::current_exception();
    }
  });
  t.join();
  CHECK(thread_ok, "thread-throw");
  try {
    std::rethrow_exception(from_thread);
  } catch (const Derived &d) {
    CHECK(d.v == 3, "thread-exception-ptr");
  }
  std::vector<std::thread> ts;
  int counts[4] = {};
  for (int i = 0; i < 4; ++i)
    ts.emplace_back([&counts, i] {
      for (int k = 0; k < 50; ++k) {
        try {
          throw_int(k);
        } catch (int v) {
          counts[i] += v == k;
        }
      }
    });
  for (auto &th : ts)
    th.join();
  CHECK(counts[0] == 50 && counts[1] == 50 && counts[2] == 50 && counts[3] == 50,
        "threads-concurrent");

  // _Unwind_Backtrace walks the whole stack.
  int bt = backtrace_depth(20);
  CHECK(bt >= 22, "unwind-backtrace");
  std::printf("INFO backtrace frames %d\n", bt);

  // Across a library (a shared library in the PIE build).
  try {
    eh_lib_throw(21);
  } catch (const eh_lib_error &e) {
    CHECK(e.code == 21, "lib-throws-to-main");
  }
  CHECK(eh_lib_catch(callback_thrower) == 1, "main-throws-through-lib");
  CHECK(eh_lib_cleanup(callback_thrower) == 1, "lib-cleanup-resume");

  std::printf("DONE eh failures=%d\n", failures);
  return failures;
}
