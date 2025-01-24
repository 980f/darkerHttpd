
/**
// Created by andyh on 1/23/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#include "darkerror.h"

#include <cerrno>
#include <cstring>
#include <cstdarg>
#include <cstdio>
// replaced err.h usage  with logging and throwing an exception.

DarkException::DarkException(int returncode, const char *msgf, ...): returncode{returncode} {
  va_list args;
  va_start(args, msgf);
  fprintf(stderr,msgf, args);//todo:1 go through program logger instead of stderr
  va_end(args);
}
const char *DarkException::what() const noexcept  {
  return strerror(returncode);
}


namespace DarkHttpd {

  /* err - prints "error: format: strerror(errno)" to stderr and exit()s with
   * the given code.
   */

  bool err(int code, const char *format, ...) {
    fprintf(stderr, "err[%d]: %s", code, code > 0 ? strerror(code) : "is not an errno.");
    va_list va;
    va_start(va, format);

    vfprintf(stderr, format, va);
    va_end(va);
    throw DarkException(code);
    return false;
  }

  void warn(const char *format, ...) {
    va_list va;
    va_start(va, format);
    vfprintf(stderr, format, va);
    fprintf(stderr, ": %s\n", strerror(errno));
    va_end(va);
  }
}
