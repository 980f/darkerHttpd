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
  clear(); //COA, including annoying client giving us more than one Range header

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

ByteRange ByteRange::canonical(__off_t st_size) {
  ByteRange resolved;
  resolved.begin.given = resolved.end.given = true;//used to signal failures or success.
  if (begin.given) {
    //was pointless to check range_end_given after checking begin, we never set the latter unless we have set the former
    if (end.given) {
      /* 100-200 */
      resolved.begin = begin;
      resolved.end = end;

      /* clamp end to filestat.st_size-1 */
      if (resolved.end > (st_size - 1)) {
        resolved.end = st_size - 1;
      }
    } else if (begin.given && !end.given) {
      /* 100- :: yields 100 to end */
      resolved.begin = begin;
      resolved.end = st_size - 1;
    } else if (!begin.given && end.given) {
      /* -200 :: yields last 200 */
      resolved.end = st_size - 1;
      resolved.begin = resolved.end - end + 1;

      /* clamp start */
      if (resolved.begin < 0) {
        resolved.begin = 0;
      }
    } else {
      resolved.begin.given = resolved.end.given = false;
      resolved.begin = resolved.end = ~0;
    }
  }
  return resolved;
}

bool ByteRange::restrictTo(const ByteRange &subset) {

  if (!subset.begin.given) {
    begin = end-subset.end;//todo: off by one
    if (begin < 0) {
      begin = 0;
    }
  }
  if (!subset.end.given) {
//todo:00 complete this logic!
  }
  return true;
}

off_t ByteRange::getLength() const {
  if (!begin.given && !end.given) {
    return 0;
  }
  if (!begin.given) {
    return end.number;
  }
  if (!end.given) {
    return -1;
  }
  return end-begin;//todo:00 off by one
}
