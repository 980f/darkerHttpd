
/**
// Created by andyh on 1/21/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#pragma once
#include <ctime>
#include "stringview.h"


/** time of latest event, in 3 formats. */
struct Now {
#define DATE_LEN 30 /* strlen("Fri, 28 Feb 2003 00:02:08 GMT")+1 */

private:
  time_t raw = 0;

public:
  // time_t utc;
  char image[DATE_LEN];

  /* Format [when] as an RFC1123 date, stored in the specified buffer.  The same
 * buffer is returned for convenience.
 */
  static char *rfc1123_date(char *dest, time_t when) {
    tm expanded;
    gmtime_r(&when, &expanded);
    //<day-name>, <day> <month> <year> <hour>:<minute>:<second> GMT
    dest[strftime(dest, DATE_LEN, "%a, %d %b %Y %H:%M:%S GMT", &expanded)] = 0; // strftime returns number of chars it put into dest.
    return dest;
  }

//  #this confuses compiler, too many candidates.
   // operator time_t() const {
   //   return raw;
   // }

  operator bool() const {
    return raw != 0;
  }

  void format() {
    rfc1123_date(image,raw);
  }

  void refresh() {
    raw = time(nullptr);
    format();
  }


  Now(time_t tv_Sec,bool textify=true) {
    raw = tv_Sec;
    if (textify) {
      format();
    } else {//forget the old
      image[0]=0;
    }
  }

  Now():Now(0,false){}

  time_t operator +(time_t other)const {
    return raw + other;
  }

  time_t operator -(time_t other)const {
    return raw - other;
  }

  time_t operator +(Now other)const {
    return raw + other.raw;
  }

  time_t operator -(Now other)const {
    return raw - other.raw;
  }


  static int monthFromAbbrev(const StringView & month);

  static int dayFromAbbrev(const StringView & day);

  void operator=(StringView timeString);

  auto operator<=>(time_t rhs) const {
    return raw-rhs;
  }

  auto operator<=>(const Now &rhs) const {
    return raw-rhs.raw;
  }

};
