
/**
// Created by andyh on 1/23/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#include "directorylisting.h"

#include <sys/stat.h>

bool DirectoryListing::operator()(const char *path, bool includeHidden) {
  DIRwrapper dir;
  if (!dir(path)) {
    return false;
  }

  dir.foreach([&](dirent *ent, const char *dirpath) {
    char currname[FILENAME_MAX]; //workspace for extended path.

    if (ent->d_name[0] == '.') {
      if (ent->d_name[1] == 0 || ent->d_name[1] == '.' || !includeHidden) {
        return; /* skip "." and ".." */
      }
    }

    snprintf(currname, sizeof currname, "%s%s", dirpath, ent->d_name); //we only call this routine if we saw a '/' at the end of the path, so we don't add one here.
    struct stat s;
    if (stat(currname, &s) == -1) {
      return; /* skip un-stat-able files */
    }
    auto repack = new dlent(); //#freed in DirectoryListing destructor
    repack->name = strdup(ent->d_name); //if we run out of heap we get mullpointer, we don't want to kill the whole app when that happens but might want to stop the listing process.
    repack->is_dir = S_ISDIR(s.st_mode);
    repack->size = s.st_size;
    repack->mtime = s.st_mtim;
    ing.push_front(repack);
  });

  ing.sort(dlent::sortOnName);
  return true;
}
