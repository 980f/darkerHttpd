
/**
// Created by andyh on 1/23/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#pragma once
#include <exception>

#include "checkFormatArgs.h"
class DarkException : public std::exception {
public:
  int returncode = 0; // what would have been returned to a shell on exit.
  /** prints to log (if enabled) when thrown, before any catching */
  DarkException(int returncode, const char *msgf, ...) checkFargs(3, 4);

  DarkException(int returncode): returncode{returncode} {}

  const char *what() const noexcept override ;
};


namespace DarkHttpd {
  // replaced err.h usage  with logging and throwing an exception.

  /* err - prints "error: format: strerror(errno)" to stderr and exit()s with
   * the given code.
   */
  bool err(int code, const char *format, ...) checkFargs(2, 3);


  void warn(const char *format, ...) checkFargs(1, 2);

};
