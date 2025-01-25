
/**
// Created by andyh on 1/21/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#pragma once
#include "stringview.h"

/** HTTP ranges header, and parser thereof */
struct ByteRange {
  struct Bound {
    off_t number; //using signed for parsing convenience.
    operator off_t() const {
      return number;
    }

    off_t operator =(long long int parsed) {
      number = parsed;
      return number;
    }

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

  ByteRange canonical(__off_t st_Size);

  ByteRange() {
    clear();
  }
};
