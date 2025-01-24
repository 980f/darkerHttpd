
/**
// Created by andyh on 1/23/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#pragma once
#include <cstdio>
#include <ctime>
#include <darkhttpd.h>

struct DarkLogger {
      bool syslog_enabled = false;
      char *file_name = nullptr; /* NULL = no logging */
      FILE *file = nullptr;

      /* open the file, perhaps emit a line to make it easy to find start and stop times */
      bool begin();
      /* close the file, perhaps with a sign-off message */
      void close();
      /** each put method knows how to format its type to the output. */
      // void put(char &&item) const;

      void put(char now) const;

      void put(const char * &&item) const;

      void put(time_t &&time);

      void put(DarkHttpd::Connection::HttpMethods method);

      void put(const StringView &view) const;

      /* todo: this guy might be intercepting and truncating longer int types. But we already expect to choke on 2Gig+ files so fixing this is not urgent */
      void put(const int &number) const;

      void put(Now &&now) {
        put(static_cast<time_t>(now));
      }

      /* output tab separated fields and a newline. No quoting is performed unless some type makes that sensible.
       * list processor template magic. Add a put() variation for any type that needs to be emitted */
      template<typename First, typename... Rest> void tsv(First &&first, Rest &&... rest) {
        put(std::forward<First>(first));
        if constexpr (sizeof...(rest) > 0) {
          put("\t"); //ambiguity between char and int resolved by this dodge.
          tsv(std::forward<Rest>(rest)...);
        } else {
          put("\n");//see \t comment.
        }
      }
    } ;
