#pragma once
#include <cstdio>
#include <unistd.h>
/**
// Created by andyh on 1/19/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

namespace DarkHttpd {
  class Fd {
  protected:
    int fd = -1;
    FILE *stream = nullptr;
    //this is the filename after createTemp is called, and it lingers after the file is closed.
    char tmpname[L_tmpnam];
  public: //temporary during code construction, this is carelessly cached.
    size_t length = 0;

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

  public:
    // ReSharper disable once CppNonExplicitConversionOperator
    operator int() const {
      return fd;
    }

    bool seemsOk() const {
      return fd != -1;
    }

    int operator=(int newfd);

    int operator=(FILE *fopened);

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

    /** lose track of the fd regardless of the associated file's state. Most uses seem like bugs, leaks of OS file handles.*/
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

    Fd() = default;

    // ReSharper disable once CppNonExplicitConvertingConstructor
    Fd(int open) : fd(open) {}

    size_t printf(const char *format, ...);
  };
}
