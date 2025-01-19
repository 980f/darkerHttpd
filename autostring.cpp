/**
// Created by andyh on 1/19/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#include "autostring.h"

#include <cctype>

#include <cstdarg>
bool AutoString::cat(const char *str, size_t len) {
  if (len == ~0) {
    len = strlen(str);
  }
  auto newLength = length + len + 1;
  auto newblock = static_cast<char *>(realloc(pointer, newLength));
  if (!newblock) { // if nullptr then old buffer is still allocated and untouched.
    return false;
  }
  // old has been freed by realloc so we don't need to also free it.
  pointer = newblock;
  // we aren't trusting the input to be null terminated at ssize
  memcpy(&pointer[length], str, len);
  length += len;
  pointer[length] = '\0';
  return true;
}

AutoString &AutoString::operator=(AutoString &other) {
  free(pointer);
  pointer = other.pointer;
  length = other.length;
  return *this;
}

AutoString &AutoString::operator=(char *replacement) {
  Free();
  pointer = replacement;
  length = replacement ? strlen(pointer) : 0;
  return *this;
}

AutoString &AutoString::toUpper() {
  for (auto scanner = pointer; *scanner;) {
    *scanner++ = toupper(*scanner);
  }
  return *this;
}

bool AutoString::malloc(size_t amount) {
  if (amount == 0) {
    return false; //don't want to find out what malloc does with a request for zero bytes.
  }
  Free();
  pointer = static_cast<char *>(::malloc(amount + 1));
  if (pointer) {
    length = amount;
    pointer[length] = '\0';
  }
  return pointer != nullptr;
}

unsigned int AutoString::ccatf(const char *format, ...) const {
  va_list args;
  va_start(args, format);
  //todo: all uses are going to go away so we aren't going to drag out vsprintf since we have such a class extending this one.
  // auto newlength=snprintf(pointer, length, format,args);
  va_end(args);
  return length;
}
//
// unsigned int Vsprinter::xvasprintf(AutoString &ret, const char *format, va_list ap) {
//   ret = nullptr; // forget the old
//   unsigned len = vasprintf(&ret.pointer, format, ap);
//   ret.length = len;
//   if (!ret || len == ~0) {
//     // errx(1, "out of memory in vasprintf()");
//   }
//   return len;
// }
// unsigned int Vsprinter::xasprintf(AutoString &ret, const char *format, ...) {
//   va_list va;
//   unsigned int len;
//
//   va_start(va, format);
//   len = xvasprintf(ret, format, va);
//   va_end(va);
//   return len;
// }
// Vsprinter::Vsprinter(const char *format, va_list ap) {
//   length = vasprintf(&pointer, format, ap);
// }
//
// Vsprinter::Vsprinter(const char *format, ...)  {
//   va_list va;
//   va_start(va, format);
//   length = vasprintf(&pointer, format, va);
//   va_end(va);
// }
