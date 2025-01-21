
/**
// Created by andyh on 1/20/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#include "base64getter.h"
unsigned Base64Getter::get6() {
  if (inputExhausted) {
    return ~0;
  }
  char b64 = encoded.chop(1);
  inputExhausted = !encoded;
  switch (b64) {
    case '=':
      inputExhausted = true;
      return ~0;
    case '/':
      return 63;
    case '+':
      return 62;
    default:
      if (b64 <= '9') {
        return b64 - '0';
      }
      if (b64 <= 'Z') {
        return b64 - 'A';
      }
      return b64 - 'a';
  }
}

uint8_t Base64Getter::operator()() {
  unsigned acc = 0;
  if (phase == 3) {
    phase = 0;
  }
  switch (phase++) {
    default: //appease compiler, won't ever happen.
    case 0: //6|2
      acc = get6() << 2;
      dregs = get6();
      return acc | (dregs >> 4);
    case 1: //4|4
      acc = dregs & 0xF << 4;
      dregs = get6();
      return acc | (dregs >> 4);
    case 2: //2|6
      acc = (dregs & 0x3) << 6;
      dregs = get6();
      return acc | dregs;
  }
}
