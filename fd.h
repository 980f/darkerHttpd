#pragma once
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
/**
// Created by andyh on 1/19/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

namespace DarkHttpd {
  /**add conveniences to file stat struct */
  struct Fstat : stat {
    void clear() {
      memset(this, 0, sizeof(stat));
    }

    Fstat() {
      clear();
    }
  };

  class Fd {
  protected:
    int fd = -1;
    Fstat filestat;
    bool statted = false;
    /* ensure stat is up-to-date*/
    bool stat() {
      return statted = (fstat(fd, &filestat) == 0);
    }

    FILE *stream = nullptr;
    //this is the filename after createTemp is called, and it lingers after the file is closed. TODO: this doesn't actually work. We will need to keep the file open when passing it to sendfile.
    char tmpname[L_tmpnam];

  public:
    FILE *getStream() { //this "create at need"
      if (fd == -1) {
        return nullptr; //todo: or perhaps spew to stderr?? This will likely segv.
      }
      if (stream == nullptr) {
        stream = fdopen(fd, "rwb"); //using b as we must use network line endings, not the platform's idea of them
      }
      return stream;
    }

    // ReSharper disable once CppNonExplicitConversionOperator
    operator int() const {
      return fd;
    }

    bool seemsOk() const {
      return fd != -1;
    }

    //attach to new fd, losing track of file status prior to this. We might someday make this autoclase if not same fd.
    int operator=(int newfd);

    /** record stream and look up its fd number. return that fd number. */
    int operator=(FILE *fopened);

    /** create a temp file via mkstemp */
    FILE *createTemp(const char *format);

    bool operator==(int newfd) const {
      return fd == newfd;
    }

    // ReSharper disable once CppNonExplicitConversionOperator
    operator bool() const {
      return fd != -1;
    }

    /** @returns whether close() thinks it worked , but we discard our fd value regardless of that */
    bool close();

    /** lose track of the fd regardless of the associated file's state. Most uses seem like bugs, potential leaks of OS file handles eventually exhausting them.*/
    void forget() {
      fd = -1;
      stream = nullptr;
    }

    /** @returns whether the fd is real and not stdin, stdout, or stderr */
    bool isNotStd() const {
      return fd > 2;
    }

    /** close this one if open, then make it attach to same file as fdother
     * @returns whether the OS was happy with doing this.
     */
    bool duplicate(int fdother) const {
      return dup2(fdother, fd) != -1; // makes fd point to same file as fdother
    }

    /** put this guy's file state into @param fdother . */
    bool copyinto(int fdother) const {
      return dup2(fd, fdother) != -1;
    }

    size_t vprintln(const char *format, va_list va);

    size_t putln(const char *content);

    void unlink() {
      //todo:close if not closed
      close();
      stream = nullptr;
    }

    off_t getLength();

    long int getPosition();

    bool isDir();

    bool isRegularFile();

    time_t getModificationTimestamp() {
      if (statted || stat()) {
        return filestat.st_mtime;
      } else {
        return 0;
      }
    }

    Fd() = default;

    // ReSharper disable once CppNonExplicitConvertingConstructor
    Fd(int open) : fd(open) {}

    size_t printf(const char *format, ...);
  };
}
