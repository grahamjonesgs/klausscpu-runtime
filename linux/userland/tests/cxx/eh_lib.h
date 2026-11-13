// The library half of eh.cpp's cross-module tests: a shared library
// (libehlib.so) in the PIE build, an object in the static one.
#pragma once

struct eh_lib_error {
  int code;
  explicit eh_lib_error(int c) : code(c) {}
  virtual ~eh_lib_error(); // key function: the type info lives in the library
};

void eh_lib_throw(int code);           // throws eh_lib_error(code)
int eh_lib_catch(void (*f)());         // 1 if f throws std::out_of_range
int eh_lib_cleanup(void (*f)());       // 1 if a destructor in the library ran
                                       // while f's exception passed through
