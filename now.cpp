
/**
// Created by andyh on 1/21/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#include "now.h"

#include <char.h>
#include <cheaptricks.h>
#include <cstdlib>
#include <cstring>

/* full set of formats:
 Sun, 06 Nov 1994 08:49:37 GMT  ; RFC 1123  char[3]=','
    Sunday, 06-Nov-94 08:49:37 GMT ; RFC 850, obsoleted by RFC 1036  char[3]!= ',' or ' '
    Sun Nov  6 08:49:37 1994       ; ANSI C's asctime() format  char[3]= ' '

    first field is ignorable
    if next field starts with digit ending space and less than 3 digits it is day of month, 4 digits is year
    if next field starts with digit ending in colon then it is HH:MM:SS and we parse the next two fields sequentially
    if next field starts with ascii and we have numbers ignore it, or stop looking when we have all the numbers
*/

int Now::monthFromAbbrev(const StringView &month) {
  static const char *monthnames[]={"JAN", "FEB", "MAR", "APR", "MAY", "JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
  return month.listIndex(monthnames,countof(monthnames));
}

int Now::dayFromAbbrev(const StringView &day) {
  static const char * daynames[]={"Sun", "Mon", "Tue", "Wed", "Thu", "Fri","Sat",};
  return day.listIndex(daynames, countof(daynames));
}

void Now::operator=(StringView timeString) {
  tm incoming;
  memset(&incoming, 0, sizeof(incoming));//better than garbage, may not be legal in all fields.
    while (timeString.length > 0) {
      auto field=timeString.cutToken(' ',true);//need true for second argument to handle ANSI C asctime
      Char firstChar=field[0];
      if (firstChar.isDigit()) {
        if (field.length==9) {//dd-mon-yy
          incoming.tm_mday=atoi(field.cutToken('-',false));
          auto month=field.cutToken('-',false);
          incoming.tm_mon=monthFromAbbrev(month);
          incoming.tm_year=atoi(field.begin());//spec is years since 1900.
          continue;
        }
        if (field.length==8) {//must be hh:mm:ss
          incoming.tm_hour=atoi(field.cutToken(':',false));
          incoming.tm_min=atoi(field.cutToken(':',false));
          incoming.tm_sec=atoi(field.begin());
          continue;
        }
        if (field.length==4) {
          incoming.tm_year=atoi(field.begin())-1900;
          continue;
        }
        if (field.length<=2) {
          incoming.tm_wday=atoi(field.begin());
          continue;
        }
      } else {
        //dow mon GMT
        auto monthindex=monthFromAbbrev(field.begin());
        if (monthindex==-1) {//not a month
          auto dayindex=dayFromAbbrev(field.chop(3));
        } else {
          incoming.tm_mon=monthindex;
        }
      }
    }
}
