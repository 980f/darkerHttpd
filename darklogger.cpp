/**
// Created by andyh on 1/23/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#include "darklogger.h"

#include <darkerror.h> //just to make failure to open log file a fatal error.
#include <iostream>
#include <sys/syslog.h>
//
// void DarkLogger::put(char item) const {
//   if (syslog_enabled) {
//     syslog(LOG_INFO, "%c", item);
//   } else if (file) {
//     fputc(item, file);
//   }
// }
//
// void DarkLogger::put(const char *item) const {
//   if (syslog_enabled) {
//     syslog(LOG_INFO, "%s", item);
//   } else if (file) {
//     fputs(item, file);
//   }
// }
//
// void DarkLogger::put(time_t time) const {
// #define CLF_DATE_LEN 29 /* strlen("[10/Oct/2000:13:55:36 -0700]")+1 */
//   char dest[CLF_DATE_LEN];
//   tm tm;
//   localtime_r(&time, &tm);
//   if (strftime(dest, CLF_DATE_LEN, "[%d/%b/%Y:%H:%M:%S %z]", &tm) == 0) {
//     dest[0] = 0;
//   }
//   put(dest);
//
// #undef CLF_DATE_LEN
// }
//
// void DarkLogger::put(StringView view) const {
//   if (syslog_enabled) {
//     syslog(LOG_INFO, "%*s", int(view.length), view.begin());
//   } else if (file) {
//     fprintf(file, "%*s", int(view.length), view.begin());
//   }
// }
//
// void DarkLogger::put(int number) const {
//   if (syslog_enabled) {
//     syslog(LOG_INFO, "%d", number);
//   } else if (file) {
//     fprintf(file, "%d", number);
//   }
// }
//
//
//
// void DarkLogger::put(Now now) const {
//   put(static_cast<time_t>(now));
// }

bool DarkLogger::begin() {
  if (file_name == nullptr) {
    os = &std::cout;
  } else {
    fstream.open(file_name);
    if (!fstream) {
      DarkHttpd::err(1, "opening logfile: fopen(\"%s\")", file_name);
      return false;
    }
    os = &fstream;
  }
  return true;
}

void DarkLogger::close() {
  fstream.flush();
  fstream.close();
}
