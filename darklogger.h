/**
// Created by andyh on 1/23/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#pragma once
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sys/syslog.h>

#include "now.h"

struct DarkLogger {
  bool syslog_enabled = false;
  char *file_name = nullptr; /* NULL = no logging */
  // FILE *file = nullptr;
  std::ofstream fstream;
  std::ostream *os = &std::cout;

  bool operator !() const {
    return !os;
  }

  /* open the file, perhaps emit a line to make it easy to find start and stop times */
  bool begin();

  /* close the file, perhaps with a sign-off message */
  void close();

  /** each put method knows how to format its type to the output. */
  // void put(char &&item) const;
  //
  // void put(char now) const;
  //
  // void put(const char *item) const;
  //
  // void put(time_t time) const;
  //
  // void put(StringView view) const;
  //
  // /* todo: this guy might be intercepting and truncating longer int types. But we already expect to choke on 2Gig+ files so fixing this is not urgent */
  // void put(int number) const;
  //
  // void put(Now now) const;

  template<typename Scalar> void put(Scalar item) {
    if constexpr (std::same_as<time_t, Scalar>) {
#define CLF_DATE_LEN 29 /* strlen("[10/Oct/2000:13:55:36 -0700]")+1 */
      char dest[CLF_DATE_LEN];
      tm tm;
      localtime_r(&item, &tm);
      if (strftime(dest, CLF_DATE_LEN, "[%d/%b/%Y:%H:%M:%S %z]", &tm) == 0) {
        dest[0] = 0;
      }
      put(dest);
#undef CLF_DATE_LEN
    } else if constexpr (std::same_as<Now, Scalar>) {
      put<time_t>(item);
    } else if constexpr (std::same_as<StringView, Scalar>) {
      if (syslog_enabled) {
        syslog(LOG_INFO, "%*s", int(item.length), item.begin());
      } else {
        *os << std::setw(item.length) << item.begin();
      }
    } else {
      *os << item;
    }
  }


  /* output tab separated fields and a newline. No quoting is performed unless some type makes that sensible.
   * list processor template magic. Add a put() variation for any type that needs to be emitted */
  template<typename First, typename... Rest> void tsv(First first, Rest... rest) {
    put(first);
    if constexpr (sizeof...(rest) > 0) {
      put('\t');
      tsv(rest...);
    } else {
      put('\n');
    }
  }
};
