/**
// Created by andyh on 1/19/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#include "fd.h"
#include <sys/stat.h>
#include <cstdarg>
#include <cstring>
#include <stdlib.h>
using namespace DarkHttpd;

int Fd::operator=(int newfd) { // NOLINT(*-unconventional-assign-operator)
  if (fd != newfd) {
    fd = newfd;
    //consider caching stream here, if fd is open.
    stream = nullptr;
    statted = false;
  }

  return newfd;
}

int Fd::operator=(FILE *fopened) {
  if (fopened) {
    stream = fopened;
    fd = fileno(stream);
    statted = false;
  }
  return fd;
}

FILE *Fd::createTemp(const char *format) {
  strncpy(tmpname, format, sizeof(tmpname));
  *this = mkstemp(tmpname);
  return getStream();
}

bool Fd::close() {
  if (seemsOk()) {
    int fi = ::close(fd);
    fd = -1;
    return fi != -1;
  }
  return true; // already closed is same as just now successfully closed.
}

size_t Fd::vprintln(const char *format, va_list va) {
  FILE *stream = getStream();
  auto added = fprintf(stream, format, va);
  fputc('\n', stream);
  return ++added;
}


size_t Fd::putln(const char *content) {
  FILE *stream = getStream();
  auto added = fputs(content, stream);
  fputc('\n', stream);
  return ++added;
}

off_t Fd::getLength() {
  if (seemsOk()) {
    if (!statted) {
      if (!stat()) {
        return -1;
      }
    }
    return filestat.st_size;
  }
  return -1;
}

long int Fd::getPosition() {
  return lseek (fd, 0, SEEK_CUR);
}

bool Fd::isDir() {
  return S_ISDIR(filestat.st_mode);
}

bool Fd::isRegularFile() {
  return !S_ISREG(filestat.st_mode);
}

size_t Fd::printf(const char *format, ...) {
  FILE *stream = getStream();
  va_list args;
  va_start(args, format);
  auto added = vfprintf(stream, format, args);
  va_end(args);
  return added;
}
