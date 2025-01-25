
/**
// Created by andyh on 1/24/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#include "mimer.h"

#include <cstring>
#include <fcntl.h>
#include <fd.h>
#include <sys/mman.h>
#include <sys/stat.h>


/* Default mimetype mappings
 * //nope: use xdg-mime on systems which have it, don't do this as the list is much larger than what we have here with lots of local user preferences.
 * //todo: only use file, with cli options to generate one from this string.
 */
static const char *default_extension_map = {
  "application/json: json\n"
  "application/pdf: pdf\n"
  "application/wasm: wasm\n"
  "application/xml: xsl xml\n"
  "application/xml-dtd: dtd\n"
  "application/xslt+xml: xslt\n"
  "application/zip: zip\n"
  "audio/flac: flac\n"
  "audio/mpeg: mp2 mp3 mpga\n"
  "audio/ogg: ogg opus oga spx\n"
  "audio/wav: wav\n"
  "audio/x-m4a: m4a\n"
  "font/woff: woff\n"
  "font/woff2: woff2\n"
  "image/apng: apng\n"
  "image/avif: avif\n"
  "image/gif: gif\n"
  "image/jpeg: jpeg jpe jpg\n"
  "image/png: png\n"
  "image/svg+xml: svg\n"
  "image/webp: webp\n"
  "text/css: css\n"
  "text/html: html htm\n"
  "text/javascript: js\n"
  "text/plain: txt asc\n"
  "video/mpeg: mpeg mpe mpg\n"
  "video/quicktime: qt mov\n"
  "video/webm: webm\n"
  "video/x-msvideo: avi\n"
  "video/mp4: mp4 m4v\n"
  "\0" //make sure that write by FD outputs a null, so we don't have to read the man page :)
};
#include <fcntlflags.h>

void Mimer::start() {
  if (!fileName) {
    return;
  }
  DarkHttpd::Fd fd(open(fileName, O_RDONLY));
  if (fd.seemsOk()) {
    struct stat filestat;
    if (fstat(fd, &filestat) == 0) {
      fileContent = StringView(static_cast<char *>(mmap(nullptr, filestat.st_size,PROT_READ,MAP_PRIVATE, fd, 0)), filestat.st_size);
    }
  } else {
    if (generate) {
      fd = open(fileName, O_REWRITE);
      if (fd.seemsOk()) {
        write(fd, default_extension_map, sizeof(default_extension_map));
      }
      fd.close();
      generate = false; //to guarantee no infinite loop as we are about to recurse to map in the defaults. Either that or we spec the 'generate' to terminate the app and make them relaunch it.
      start(); //
    }
  }
  //it is ok to let fd close, the mmap persists until process end or munmap.
}

void Mimer::finish() {
  munmap(fileContent.pointer, fileContent.length);
}

const char *Mimer::operator()(const char *url) {
  if (url) {
    if (auto period = strrchr(url, '.')) {
      StringView extension = const_cast<char *>(++period);
      StringView pool = fileContent ? fileContent : StringView(const_cast<char *>(default_extension_map)); //the const_cast seems dangerous, but we will get a segv if we screw up, we will not get a silent bug.
      ssize_t foundAt = pool.findLast(extension);
      if (foundAt >= 0) {
        //todo: idiot check that this extension is not in the middle of any other extension nor inside a mimetype
        auto mimeEnd = pool.lookBack(foundAt, ':');
        if (mimeEnd) {
          auto mimeStart = pool.lookBack(mimeEnd, '\n');
          if (!mimeStart) {
            mimeStart = 0;
          }
          return pool.subString(mimeStart, mimeEnd);
        }
      }
    }
  }
  /* no period found in the string or extension not found in map */
  return default_type ? default_type : "application/octet-stream";
}
