
/**
// Created by andyh on 1/21/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#include "byterange.h"

#include <cheaptricks.h>

/* Parse a Range: field into range_begin and range_end.  Only handles the
 * first range if a list is given.  Sets range_{begin,end}_given to true if
 * associated part of the range is given.
 * "Range: bytes=500-999"
 * "Range: - 456 last 456
 * "Range: 789 -  from 789 to end
 */

int ByteRange::parse(StringView rangeline) {
  //todo: allow range operand type default?
  auto prefix = rangeline.cutToken('=', false);
  if (!prefix) {
    return 498;
  }
  if (prefix != "bytes") {
    return 498; //todo: error range format not supported
  }
  recycle(); //COA, including annoying client giving us more than one Range header

  begin = rangeline.cutNumber();
  if (begin < 0) { // e.g. -456
    end = -take(begin);
    end.given = true;
  } else {
    begin.given = true;
    if (rangeline[0] == '-') {
      end = rangeline.cutNumber();
      end.given = end > 0;
    }
    if (begin.given && end.given) {
      if (end < begin) {
        return 497;
      }
    }
  }
  //an additional range is presently not supported, we should return an error response.
  return 499; //todo: proper value
}
