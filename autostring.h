#pragma once
#include <cstdlib>
#include <cstring>
#include <stringview.h>
#include "checkFormatArgs.h"
/**
// Created by andyh on 1/19/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/


/** to help ensure timely frees.
 * Many people seem to not know that 'free' doesn't mind null pointers, it checks for them and does nothing so that we don't have to do that in a gazillion places.
 *
 * This frees what it has been given at constructor or by assignment when there is an assignment or a destruction. As such never assign to it from anything not malloc'd.
 */
struct AutoString : StringView {
  // char *operator[](size_t offset)  {
  //   static char fakechar;
  //   if (!pointer) {
  //     return &fakechar;
  //   }
  //   return &pointer[offset];
  // }


  /**
   * @param str to append to end after reallocating room for it
   * @param len length of string if known, ~0 for unknown (default)
   * @returns whether the append happened, won't if out of heap
   */
  bool cat(const char *str, size_t len = ~0);

  /** construct around an allocated content. */
  AutoString(char *pointer = nullptr, unsigned length = ~0) : StringView(pointer, length) {
    if (pointer && length == ~0) {
      this->length = strlen(pointer);
    }
  }

  void Free() {
    free(pointer);
    // null fields in case someone tries to use this after it is deleted
    pointer = nullptr;
    length = 0;
  }

  ~AutoString() {
    Free();
  }

  AutoString &operator=(AutoString &other);

  AutoString &operator=(char *replacement);

  AutoString &toUpper();

  /** frees present content and mallocs @param amount bytes plus one for a null that it inserts. */
  bool malloc(size_t amount);

  AutoString(AutoString &&other) = delete;

  AutoString(const StringView &view) : StringView{static_cast<char *>(::malloc(view.length + 1)), view.length} {
    memcpy(pointer, view.begin(), view.length);
    //todo: add terminating null
  }

  unsigned int ccatf(const char *format, ...) const;
};

//
// /** delete vasprintf allocation when exit scope.
//  * vasprintf() internally allocates from heap, returning via a pointer to pointer with a length by normal return
//   */
// struct Vsprinter : AutoString {
//   /* vasprintf() that dies if it fails. */
//   static unsigned int xvasprintf(AutoString &ret, const char *format, va_list ap) checkFargs(2, 0);
//
//   /* asprintf() that dies if it fails. */
//   static unsigned int xasprintf(AutoString &ret, const char *format, ...) checkFargs(2, 3);
//
//   Vsprinter(const char *format, va_list ap);
//
//   Vsprinter (const char *format, ...) checkFargs(2, 3) ;
// };
