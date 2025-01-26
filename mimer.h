
/**
// Created by andyh on 1/24/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#pragma once
#include <stringview.h>


struct Mimer {
  const char *default_type = nullptr;
  /** file of mime mappings gets read into here.*/
  StringView fileContent = nullptr;
  /** file to load mime types map from */
  char *fileName = nullptr;

  //access the file
  void start();

  const char *operator()(const char *url);

  //release OS resources
  void finish();

  /** cli request to set given file to the internal set.
   * Presently it is acted upon by start().
   */
  bool generate;
};
