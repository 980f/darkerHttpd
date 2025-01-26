/**
// Created by andyh on 1/21/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#pragma once
#include "stringview.h"

/** started as HTTP ranges header, and parser thereof. Gets used for file transmission state tracking.*/
struct ByteRange {
  struct Bound {
    off_t number; //using signed type for parsing convenience.
    operator off_t() const {
      return given ? number : ~0;
    }

    off_t operator =(long long int parsed) {
      number = parsed;
      given = parsed != ~0;
      return number;
    }

    //exposed for legacy reasons.
    bool given;

    void clear() {
      number = 0;
      given = false;
    }
  };

  Bound begin;
  Bound end;

  int parse(StringView headerline);

  void clear() {
    begin.clear();
    end.clear();
  }

  /** set to definitely 0 to @param allofit */
  ByteRange &setForSize(size_t allofit) {
    begin = 0;
    end = allofit;
    return *this;
  }

  /** convert 'not given' to a concrete value such as 0 */
  ByteRange canonical(__off_t st_Size);

  ByteRange() {
    clear();
  }
};
