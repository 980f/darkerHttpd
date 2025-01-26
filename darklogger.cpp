/**
// Created by andyh on 1/23/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#include "darklogger.h"

#include <darkerror.h> //just to make failure to open log file a fatal error.

bool DarkLogger::begin() {
  if (file_name == nullptr) {
    os = &std::cout;
  } else {
    fstream.open(file_name);
    if (!fstream) {
      DarkHttpd::err(1, "opening logfile: fopen(\"%s\")", file_name);
      return false;
    }
    os = &fstream;
  }
  return true;
}

void DarkLogger::close() {
  fstream.flush();
  fstream.close();
}
