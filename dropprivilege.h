
/**
// Created by andyh on 1/24/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#pragma once


struct DropPrivilege {
  bool asGgroup;
  char *byName = nullptr;
  unsigned byNumber;

  bool operator !() const {
    return byName != nullptr;
  }

  operator unsigned() const {
    return byNumber;
  }

  const char *typeName();

  bool validate();

  void operator=(char *arg);

  bool operator()();
};
