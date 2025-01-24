
/**
// Created by andyh on 1/23/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#pragma once
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <forward_list>
#include <functional>

/** manage lifetime of libc directory lister, can live outside this namespace */
  struct DIRwrapper {
    DIR *raw;
    const char *path = nullptr;

    ~DIRwrapper() {
      closedir(raw);
    }

    bool operator ()(const char *path) {
      raw = opendir(path);
      return raw != nullptr;
    }

    dirent *next() {
      return readdir(raw);
    }

    void foreach(std::function<void(dirent *, const char *)> body) {
      while (auto entry = next()) {
        body(entry, path);
      }
    }
  };

  /* listing mechanism, a reworking of what stat returns for a name */
  class DirectoryListing {
    struct dlent {
      char *name = nullptr; /* The strdup'd name/path of the entry.       */
      bool is_dir = false; /* If the entry is a directory and not a file. */
      size_t size = 0; /* The size of the entry, in bytes.            */
      timespec mtime; /* When the file was last modified.            */

      dlent(): mtime{0, 0} {}

      ~dlent() {
        free(name);
      }

      static int sortOnName(dlent *a, dlent *b) {
        return strcmp(a->name, b->name);
      }

      //can add other standard sorts here.
    };

  public:
    std::forward_list<dlent *> ing; //this name will make sense at point of use.

    ~DirectoryListing() {
      for (auto dlent: ing) {
        delete(dlent);
      }
      ing.clear();
    }

  public:

    /* Make sorted list of files in a directory.  Returns number of entries, or -1
     * if error occurs.
     */
    bool operator()(const char *path, bool includeHidden);
  };
