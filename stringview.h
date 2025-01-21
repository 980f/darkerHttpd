#pragma once
#include <cstdio>
/**
// Created by andyh on 1/18/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

/** a not-null terminated string that can be shrunk but not expanded.
 * This class exists so that we can sub-string without touching the original string OR copying it. It is a bridge while weaning Dark(er)Httpd from using dynamically mucked with strings.
 * StringView may copy its content to new views, which can then result in use-after-free if the StringView itself was created with a pointer that gets freed.
 */
struct StringView {
  char *pointer = nullptr;
  size_t length = 0;
  size_t start = 0;

  StringView(char *pointer, size_t length = ~0, size_t start = 0);

  StringView(char *begin, char *pastEnd): pointer{begin}, length(pastEnd > begin ? pastEnd - begin : 0) {}

  StringView &operator=(char *str);

  char *begin() const {
    return &pointer[start];
  }

  /* @returns a pointer to the char just past the end,  ie you should use this with a pre-decrement if doing a search.*/
  char *end(unsigned offset = 0) {
    return &pointer[length - offset];
  }

  /** case insensitive compare */
  bool operator==(const char *toMatch) const;

  /** case insensitive compare */
  bool operator==(const StringView& toMatch) const;

  //todo:1 exact string compares, not ignoring case.


  operator char *() const {
    return begin();
  }

  bool notTrivial() const {
    return pointer && length > 0;
  }

  /** @returns whether we have a nontrivial block*/
  operator bool() const {
    return notTrivial();
  }

  bool operator!() const {
    return pointer == nullptr || length == 0;
  }

  StringView subString(size_t start, size_t pastEnd) const;

  /** @returns pointer to byte after where this StringView's content was inserted. If this guy is trivial then this will be the same as @param bigenough which is where the contents of this object are copied into.
   * @param honorNull is whether to stop inserting this guy if a null is found in this short of the length. Such a null is NOT copied into bigenough.
   * Typical use is to assign a null to the returned value  *view.put(target,true)=0;
   */
  char *put(char *bigenough, bool honorNull) const;

  /* @returns index of first instance of @param sought found looking backwards from @param searchpoint not including searchpoint itself, -1 if char not found. */
  ssize_t lookBack(ssize_t searchpoint, char sought) const;

  ssize_t lookAhead(char sought) const;

  /** move the start by @param moveStart, equivalent to removing the front of the string. */
  StringView chop(size_t moveStart);

  StringView cutToken(char termchar, bool orToEnd);

  unsigned listIndex(const char *list[], unsigned countOfList) const;

  /** calls @see put then adds a terminator and returns a pointer to that terminator. */
  char *cat(char *bigenough, bool honorNull) const {
    auto end = put(bigenough, honorNull);
    if (end) {
      *end = 0;
    }
    return end;
  }

  size_t put(FILE *out) const {
    return fwrite(pointer, length, 1, out); //#fputs requires a null terminator, this class exists to support not-null-terminated char arrays.
  }

  void trimTrailing(const char *trailers);

  void trimLeading(const char *trailers);

  char &operator[](size_t offset) {
    // static char buggem;
    if (start + offset >= length) {
      return *begin(); //bad offset will point to first char which should surprise the programmer. The author of this code never makes this kind of error, he adds methods to this class for anything complex.
    }
    return pointer[start + offset];
  }

  /**
    * @param str is this what the string ends with?
    * @param len length if known, else leave off or pass ~0 and strlen will be called on str
    * @returns whether the internal string ends with str .
    */
  bool endsWith(const char *str, unsigned len = ~0) const;

  /** @returns whether last char is @param slash*/
  bool endsWith(char slash) const {
    return pointer && length > 0 && slash == pointer[length - 1];
  }

  ssize_t findLast(const StringView &extension) const;

  long long int cutNumber();

  /** @wraps strchr */
  char *find(char c);

  void truncateAt(char *writer);
};
