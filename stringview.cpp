/**
// Created by andyh on 1/18/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#include "stringview.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

StringView::StringView(char *pointer, size_t length, size_t start): pointer{pointer},
  length{length},
  start{start} {
  if (pointer && length == ~0) {
    this->length = strlen(&pointer[start]);
  }
}

StringView &StringView::operator=(char *str) {
  pointer = str;
  length = strlen(str);
  start = 0;
  return *this;
}

bool StringView::operator==(const char *toMatch) const {
  return strncasecmp(begin(), toMatch, length) == 0;
}

bool StringView::operator==(const StringView &toMatch) const {
  return (strncasecmp(begin(), toMatch.begin(), std::min(length, toMatch.length)) == 0) && length == toMatch.length;
}

StringView StringView::subString(size_t start, size_t pastEnd) const {
  //todo: argument checks, both must be less than length and their sum must be less than length.
  if (start + pastEnd > length) {
    return StringView(pointer + this->start + start, pastEnd - start);
  }
  return StringView(nullptr, 0, 0);
}

char *StringView::put(char *bigenough, bool honorNull) const {
  if (bigenough) {
    if (pointer) {
      if (length) {
        for (size_t count = 0; count < length; ++count) {
          char c = pointer[start + count];
          if (honorNull && !c) {
            break;
          }
          *bigenough++ = c;
        }
      }
    }
  }
  return bigenough;
}

ssize_t StringView::lookBack(ssize_t searchpoint, char sought) const {
  if (searchpoint > length) {
    return -1; //Garbage in: act dumb.
  }
  while (--searchpoint >= 0) {
    if (pointer[start + searchpoint] == sought) {
      break;
    }
  }
  return searchpoint;
}

ssize_t StringView::lookAhead(char sought) const {
  size_t looker = 0;
  do {
    if (pointer[start + looker] == sought) {
      return looker;
    }
  } while (++looker < length) ;
  return -1;
}

StringView StringView::chop(size_t moveStart) {
  StringView discard(begin(), moveStart);
  if (moveStart > length) {
    discard.length = length;
    start = length;
    length = 0;
  } else {
    start += moveStart;
    length -= moveStart;
  }
  return discard;
}

StringView StringView::cutToken(char termchar, bool orToEnd) {
  if (notTrivial()) {
    auto cutpoint = lookAhead(termchar);
    if (cutpoint == -1) {
      if (orToEnd) {
        StringView token = StringView(begin(), length); //limit new view as much as possible, no looking back in front of it.
        start = length; //null this one, but don't muck with its pointer as we may be an AutoString
        return token;
      } else {
        return StringView(nullptr, 0, 0);
      }
    } else {
      StringView token = chop(cutpoint); //remove termchar along with what preceded it.
      StringView discard=chop(1);//the termchar
      *discard.begin()=0;//null the token.
      return token;
    }
  }
  return StringView(nullptr);
}

unsigned StringView::listIndex(const char *list[], unsigned countOfList) const {
  for (unsigned i = countOfList; i-- > 0;) {
    if (*this == list[i]) {
      return i;
    }
  }
  return ~0; //should blow things up nicely if not checked.
}

void StringView::trimTrailing(const char *trailers) {
  if (notTrivial()) {
    while (length && strchr(trailers, pointer[start + length - 1])) {
      --length;
    }
  }
}

void StringView::trimLeading(const char *trailers) {
  if (notTrivial()) {
    while (length && strchr(trailers, pointer[start])) {
      ++start;
      --length;
    }
  }
}

// keep for a little while, it was needed at one time
// /** @returns nullptr if the line is blank or EOL or the EOL comment char, else points to first char not a space nor a tab */
// static const char *removeLeadingWhitespace(const char *text) {
//   auto first = strspn(text, " \t\n\r");
//
//   while (auto c = text[first]) {
//     switch (c) {
//       case ' ':
//       case '\t':
//         continue;
//       case 0: //text is all blanks or tabs, index is of the null terminator
//       case '#': //EOL comment marker
//       case '\r': //in case we pull these out of
//       case '\n': // ... the whitespace string
//         return nullptr;
//       default:
//         return &text[first];
//     }
//   }
//   return nullptr;
// }


bool StringView::endsWith(const char *str, unsigned len) const {
  if (len == ~0) {
    len = strlen(str);
  }
  return length > len && memcmp(&pointer[length - len], str, len) == 0;
}

ssize_t StringView::findLast(const StringView &extension) const {
  if (notTrivial() & extension.notTrivial()) { //without checking extension length we could seek starting one byte past our allocation, probably harmless but easier to preclude than to verify.
    if (extension.length < length) {
      auto given = pointer + length - extension.length;

      given = strrchr(given, ' '); //todo: allow tabs
      if (given) {
        if (strncmp(extension.begin(), given, extension.length) == 0) {
          return given - begin();
        }
      }
    }
  }
  return -1;
}

long long int StringView::cutNumber() {
  char *start = begin();
  char *end;
  auto number = strtoll(start, &end, 10);
  if (end == start) {
    //nothing was there, no number was parsed
    return 0;
  }
  chop(end - start);
  return number;
}

char *StringView::find(char c) {
  for (char *scanner = begin(); scanner != end(); scanner++) {
    if (*scanner == c) {
      return scanner;
    }
  }
  return nullptr;
}

void StringView::truncateAt(char *writer) {
  if (writer >= begin()) {
    if (writer < end()) {
      *writer = '\0';
      length = writer - begin();
    }
  }
}
