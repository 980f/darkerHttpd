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
