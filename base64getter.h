
/**
// Created by andyh on 1/20/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#pragma once
#include <cstdint>
#include "stringview.h"

/** pull sequentially from this, which pulls sequentially from a StringView */
class Base64Getter {
  StringView encoded;
  unsigned phase = 0;
  char dregs = ~0;
  bool inputExhausted = false;

  unsigned get6();
public:
  Base64Getter(const StringView &encoded) : encoded{encoded} {}

  operator bool() const {
    return !inputExhausted;
  }

  uint8_t operator()();
};
