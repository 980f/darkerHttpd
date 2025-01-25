
/**
// Created by andyh on 1/24/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#include "dropprivilege.h"

#include <cstdio>
#include <cstdlib> //atoi
#include <darkerror.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>


const char *DropPrivilege::typeName() {
  return asGgroup ? "gid" : "uid";
}

bool DropPrivilege::validate() {
  if (asGgroup) {
    group *g = getgrnam(byName);
    if (!g) {
      g = getgrgid(atoi(byName));
    }
    if (g) {
      byNumber = g->gr_gid;
      return true;
    }
  } else {
    passwd *p = getpwnam(byName);
    if (!p) {
      p = getpwuid(atoi(byName));
    }
    if (p) {
      byNumber = p->pw_uid;
      return true;
    }
  }
  DarkHttpd::err(-1, "no such %s: `%s'", typeName(), byName);
  return false;
}

bool DropPrivilege::operator()() {
  validate();
  int setit = -1; //init to 'failed'

  if (asGgroup) {
    gid_t list[1] = {byNumber};
    if (setgroups(1, list) == -1) { //todo: this seems aggressive/intrusive, I'd rather have the gid drop fail if the user is not already in that supposedly limited group.
      DarkHttpd::err(1, "setgroups([%u])", byNumber);
      return false;
    }
    setit = setgid(byNumber);
  } else {
    setit = setuid(byNumber);
  }
  if (setit == -1) {
    DarkHttpd::err(1, "set%s(%u)", asGgroup ? "gid" : "uid", byNumber);
    return false;
  }

  printf("set %s to %u\n", asGgroup ? "gid" : "uid", byNumber);
  return true;
}

void DropPrivilege::operator=(char *arg) {
  byName = arg;
  //maybe: validate()
  byNumber = atoi(arg); //unchecked conversion
}
