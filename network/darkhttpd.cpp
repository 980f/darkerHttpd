/**
// Created by andyh on 12/14/24.
// Copyright (c) 2024 Andy Heilveil, (github/980f). All rights reserved.

 * This module was started using C source whose license was:
 *
 * darkhttpd - a simple, single-threaded, static content webserver.
 * https://unix4lyfe.org/darkhttpd/
 * Copyright (c) 2003-2024 Emil Mikulic <emikulic@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 * It has been heavily modified to make it a module that can be included in other programs.
 * todo: replace strduping the mimemap contents with loading and keeping the whole external mime list, malloc once instead of dozens of times.
 * todo: replace strduping string values extracted from the request, instead poke nulls and point into it.
 *
*/

#include "darkhttpd.h"

#include <iostream>


static const char pkgname[] = "darkhttpd/1.16.from.git/980f";
static const char copyright[] = "copyright (c) 2003-2024 Emil Mikulic"
  ", totally refactored in 2024 by github/980f";

/* Possible build options: -DDEBUG -DNO_IPV6 */

#define _FILE_OFFSET_BITS 64 /* stat() files bigger than 2GB */
#include <sys/sendfile.h>

#ifdef __sun__
#include <sys/sendfile.h>
#endif

#include <arpa/inet.h>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pwd.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

class DebugLog {
  bool spew = false;

public:
  /** @returns spew so that you can do if(debug ()) {  more behavior conditional on debug }*/
  bool operator()(const char *format, va_list &vargs) {
    if (spew && format) {
      vfprintf(stdout, format, vargs);
    }
    return spew;
  }

  bool operator()(const char *format, ...) const checkFargs(2, 3) {
    if (spew && format) {
      va_list args;
      va_start(args, format);
      vfprintf(stdout, format, args);
      va_end(args);
    }
    return spew;
  }

  bool operator=(bool enable) {
    spew = enable;
    return enable;
  }

  DebugLog(bool startup) : spew(startup) {}
};

#define countOf(a) (sizeof(a) / sizeof(*a))

// It is best to always have the debug statements present even when it is disabled, to reduce the difference between debug build execution and 'release' builds.
#ifndef DEBUG
#define NDEBUG
static DebugLog debug(false);
#else
static DebugLog debug(true);
#endif


class DarkException : public std::exception {
public:
  int returncode = 0; // what would have been returned to a shell on exit.
  /** prints to log (if enable) when thrown, before any catching */
  // DarkException(int returncode, const char *msgf, ...) checkFargs(3, 4) : returncode{returncode} {
  //   va_list args;
  //   va_start(args, msgf);
  //   debug(msgf, args);
  //   va_end(args);
  // }
  //
  DarkException(int returncode): returncode{returncode} {}

  const char *what() const noexcept override {
    return strerror(returncode);
  }
};

/** The time formatting that we use in directory listings.
 * An example of the default is 2013-09-09 13:01, which should be compatible with xbmc/kodi. */
static const char *const DIR_LIST_MTIME_FORMAT = "%Y-%m-%d %R";
static const unsigned DIR_LIST_MTIME_SIZE = 16 + 1; /* How large the buffer will need to be. */

/* This is for non-root chroot support on FreeBSD 14.0+ */
/* Must set sysctl security.bsd.unprivileged_chroot=1 to allow this. */
#ifdef __FreeBSD__
#if __FreeBSD_version >= 1400000
#define HAVE_NON_ROOT_CHROOT
#endif
#endif

/* https://github.com/hboetes/mg/issues/7#issuecomment-475869095 */
#if defined(__APPLE__) || defined(__NetBSD__)
#define st_atim st_atimespec
#define st_ctim st_ctimespec
#define st_mtim st_mtimespec
#endif

#ifdef HAVE_NON_ROOT_CHROOT
#include <sys/procctl.h>
#endif


#if __has_include(<sanitizer/msan_interface.h>)
#include <sanitizer/msan_interface.h>
#endif

#ifdef __sun__
#ifndef INADDR_NONE
#define INADDR_NONE -1
#endif
#endif

#ifndef MAXNAMLEN
#ifdef NAME_MAX
#define MAXNAMLEN NAME_MAX
#else
#define MAXNAMLEN 255
#endif
#endif

#if defined(O_EXCL) && !defined(O_EXLOCK)
#define O_EXLOCK O_EXCL
#endif


#if defined(__GNUC__) || defined(__INTEL_COMPILER)
#define unused __attribute__((__unused__))
#else
#define unused
#endif

static_assert(sizeof(unsigned long long) >= sizeof(off_t), "inadequate ull, not large enough for an off_t");


//for printf, make it easy to know which token to use by forcing the data to the largest integer supported by it.
template<typename Integrish> auto llu(Integrish x) {
  return static_cast<unsigned long long>(x);
}

// replaced err.h usage  with logging and throwing an exception.

/* err - prints "error: format: strerror(errno)" to stderr and exit()s with
 * the given code.
 */
static void err(int code, const char *format, ...) checkFargs(2, 3);

static void err(int code, const char *format, ...) {
  fprintf(stderr, "err[%d]: %s", code, code > 0 ? strerror(code) : "is not an errno.");
  va_list va;
  va_start(va, format);

  vfprintf(stderr, format, va);
  va_end(va);
  throw DarkException(code);;
}

#define errx(first, second, ... ) err( - (first), second, ## __VA_ARGS__ )

static void warn(const char *format, ...) checkFargs(1, 2);

static void warn(const char *format, ...) {
  va_list va;
  va_start(va, format);
  vfprintf(stderr, format, va);
  fprintf(stderr, ": %s\n", strerror(errno));
  va_end(va);
}

/** @returns nullptr if the line is blank or EOL or EOL comment char, else points to first char not a space nor a tab */
static const char *removeLeadingWhitespace(const char *text) {
  auto first = strspn(text, " \t\n\r");

  while (auto c = text[first]) {
    switch (c) {
      case ' ':
      case '\t':
        continue;
      case 0: //text is all blanks or tabs, index is of the null terminator
      case '#': //EOL comment marker
      case '\r': //in case we pull these out of
      case '\n': // ... the whitespace string
        return nullptr;
      default:
        return &text[first];
    }
  }
  return nullptr;
}


/* close() that dies on error.  */
static void xclose(DarkHttpd::Fd fd) {
  if (!fd.close()) {
    err(1, "close()");
  }
}

// removed  strntoupper as we are quite careful to enforce nulls on the end of anything we upperify.
/* malloc that dies if it can't allocate. */
static void *xmalloc(const size_t size) {
  void *ptr = malloc(size);
  if (ptr == nullptr) {
    errx(1, "can't allocate %zu bytes", size);
  }
  return ptr;
}

/* realloc() that dies if it can't reallocate. */
static void *xrealloc(void *original, const size_t size) {
  void *ptr = realloc(original, size);
  if (ptr == nullptr) {
    errx(1, "can't reallocate %zu bytes", size);
  }
  return ptr;
}

/* strdup() that dies if it can't allocate.
 * Implement this ourselves since regular strdup() isn't C89.
 */
static char *xstrdup(const char *src) {
  size_t len = strlen(src) + 1;
  char *dest = static_cast<char *>(xmalloc(len));
  if (dest != nullptr) {
    memcpy(dest, src, len);
  }
  return dest;
}

/** delete vasprintf allocation when exit scope.
 * vasprintf() internally allocates from heap, returning via a pointer to pointer with a length by normal return
  */
struct Vsprinter : AutoString {
  Vsprinter(const char *format, va_list ap) {
    length = vasprintf(&pointer, format, ap);
  }

  Vsprinter(const char *format, ...) checkFargs(2, 3) {
    va_list va;
    va_start(va, format);
    length = vasprintf(&pointer, format, va);
    va_end(va);
  }
};

/* vasprintf() that dies if it fails. */
static unsigned int xvasprintf(AutoString &ret, const char *format, va_list ap) checkFargs(2, 0);

static unsigned int xvasprintf(AutoString &ret, const char *format, va_list ap) {
  ret = nullptr; // forget the old
  unsigned len = vasprintf(&ret.pointer, format, ap);
  ret.length = len;
  if (!ret || len == ~0) {
    errx(1, "out of memory in vasprintf()");
  }
  return len;
}

/* asprintf() that dies if it fails. */
static unsigned int xasprintf(AutoString &ret, const char *format, ...) checkFargs(2, 3);

static unsigned int xasprintf(AutoString &ret, const char *format, ...) {
  va_list va;
  unsigned int len;

  va_start(va, format);
  len = xvasprintf(ret, format, va);
  va_end(va);
  return len;
}

/* Append buffer code.  A somewhat efficient string buffer with pool-based
 * reallocation.
 */
#ifndef APBUF_INIT
#define APBUF_INIT 4096
#endif

// the below should probably be configurable rather than hard coded at same size as default init.
#define APBUF_GROW APBUF_INIT

struct apbuf { // this looks like an std::vector<char>
  size_t length;
  size_t pool;
  char *str;

  apbuf() {
    length = 0;
    pool = APBUF_INIT;
    str = static_cast<char *>(xmalloc(pool));
  }

  /* Append s (of length len) to buf. */
  void appendl(const char *s, const size_t len) {
    size_t need = length + len;
    if (pool < need) {
      /* pool has dried up */
      while (pool < need) {
        pool += APBUF_GROW;
      }
      str = static_cast<char *>(xrealloc(str, pool));
    }
    memcpy(str + length, s, len);
    length += len;
  }

  void appendl(const Vsprinter &vs) {
    appendl(vs.pointer, vs.length);
  }

  void appendf(const char *format, ...) checkFargs(2, 3) {
    va_list va;
    va_start(va, format);
    appendl(Vsprinter(format, va));
    va_end(va);
  }
};

// the following is a minor performance enhancement:
#ifdef __GNUC__
#define append(buf, s) buf->appendl(s, (__builtin_constant_p(s) ? sizeof(s) - 1 : strlen(s)))
#else
static void append(struct apbuf *buf, const char *s) {
  buf->appendl(s, strlen(s));
}
#endif

/* Make the specified socket non-blocking. */
static void nonblock_socket(const int sock) {
  int flags = fcntl(sock, F_GETFL);

  if (flags == -1) {
    err(1, "fcntl(F_GETFL)");
  }
  flags |= O_NONBLOCK;
  if (fcntl(sock, F_SETFL, flags) == -1) {
    err(1, "fcntl() to set O_NONBLOCK");
  }
}

/* strdup a sub string [begin:end), adding a null. */
static char *sub_string(const char *begin, const char *end) {
  if (end) {
    auto length = end - begin;
    char *dest = new char [length + 1]; //+1 for null
    memcpy(dest, begin, length);
    dest[length] = '\0';
    return dest;
  }
  return strdup(begin);
}

/* Resolve /./ and /../ in a URL, in-place.
 * Returns NULL if the URL is invalid/unsafe, or the original buffer if
 * successful.
 */
static char *make_safe_url(char *const url) {
  char *src = url;
  char *dst;
#define ends(c) ((c) == '/' || (c) == '\0')

  /* URLs not starting with a slash are illegal. */
  if (*src != '/') {
    return nullptr;
  }

  /* Fast case: skip until first double-slash or dot-dir. */
  for (; *src; ++src) {
    if (*src == '/') {
      if (src[1] == '/') {
        break;
      } else if (src[1] == '.') {
        if (ends(src[2])) {
          break;
        } else if (src[2] == '.' && ends(src[3])) {
          break;
        }
      }
    }
  }

  /* Copy to dst, while collapsing multi-slashes and handling dot-dirs. */
  dst = src;
  while (*src) {
    if (*src != '/') {
      *dst++ = *src++;
    } else if (*++src == '/');
    else if (*src != '.') {
      *dst++ = '/';
    } else if (ends(src[1])) {
      /* Ignore single-dot component. */
      ++src;
    } else if (src[1] == '.' && ends(src[2])) {
      /* Double-dot component. */
      src += 2;
      if (dst == url) {
        return nullptr; /* Illegal URL */
      } else {
        /* Backtrack to previous slash. */
        while (*--dst != '/' && dst > url);
      }
    } else {
      *dst++ = '/';
    }
  }

  if (dst == url) {
    ++dst;
  }
  *dst = '\0';
  return url;
#undef ends
}

bool AutoString::endsWith(const char *str, unsigned len) const {
  if (len == ~0) {
    len = strlen(str);
  }
  return length > len && memcmp(&pointer[length - len], str, len) == 0;
}

bool AutoString::cat(const char *str, size_t len) {
  if (len == ~0) {
    len = strlen(str);
  }
  auto newLength = length + len + 1;
  auto newblock = static_cast<char *>(realloc(pointer, newLength));
  if (!newblock) { // if nullptr then old buffer is still allocated and untouched.
    return false;
  }
  // old has been freed by realloc so we don't need to also free it.
  pointer = newblock;
  // we aren't trusting the input to be null terminated at ssize
  memcpy(&pointer[length], str, len);
  length += len;
  pointer[length] = '\0';
  return true;
}

AutoString &AutoString::operator=(AutoString &other) {
  free(pointer);
  pointer = other.pointer;
  length = other.length;
  return *this;
}

AutoString &AutoString::operator=(char *replacement) {
  Free();
  pointer = replacement;
  length = replacement ? strlen(pointer) : 0;
  return *this;
}

AutoString &AutoString::toUpper() {
  for (auto scanner = pointer; *scanner;) {
    *scanner++ = toupper(*scanner);
  }
  return *this;
}

bool AutoString::malloc(size_t amount) {
  if (amount == 0) {
    return false; //don't want to find out what malloc does with a request for zero bytes.
  }
  Free();
  pointer = static_cast<char *>(::malloc(amount + 1));
  if (pointer) {
    length = amount;
    pointer[length] = '\0';
  }
  return pointer != nullptr;
}

char *StringView::put(char *bigenough, bool honorNull) const {
  if (bigenough) {
    if (pointer) {
      if (length) {
        for (size_t count = 0; count < length; ++count) {
          *bigenough++ = pointer[count];
          if (honorNull && !pointer[count]) {
            break;
          }
        }
      }
    }
    //this needs to be conditional for when we insert this guy into a right-sized hole inside *bigenough=0;
  }
  return bigenough;
}

char *StringView::cat(char *bigenough, bool honorNull) const {
  auto end = put(bigenough, honorNull);
  if (end) {
    *end = 0;
  }
  return end;
}

size_t StringView::put(FILE *out) const {
  return fwrite(pointer, length, 1, out);
}

void StringView::trimTrailing(const char *trailers) {
  if (pointer == nullptr) {
    return;
  }
  while (length && strchr(trailers, pointer[start + length - 1])) {
    --length;
  }
}

Inaddr6 & Inaddr6::clear() {
  for (auto index = 4; index-- > 0;) { //gcc converts this to a memset.
    __in6_u.__u6_addr32[index] = 0;
  }
  return *this;
}

bool Inaddr6::isUnspecified() const {
  return __in6_u.__u6_addr32[0] == 0 && __in6_u.__u6_addr32[1] == 0 && __in6_u.__u6_addr32[2] == 0 && __in6_u.__u6_addr32[3] == 0;
}

bool Inaddr6::isLoopback() const {
  return __in6_u.__u6_addr32[0] == 0 && __in6_u.__u6_addr32[1] == 0 && __in6_u.__u6_addr32[2] == 0 && __in6_u.__u6_addr32[3] == htonl(1);
}

bool Inaddr6::isLinkLocal() const {
  return __in6_u.__u6_addr32[0] & htonl(0xffc00000) == htonl(0xfe800000);
}

bool Inaddr6::isSiteLocal() const {
  return __in6_u.__u6_addr32[0] & htonl(0xffc00000) == htonl(0xfec00000);
}

bool Inaddr6::wasIpv4() const {
  return __in6_u.__u6_addr32[0] == 0 && __in6_u.__u6_addr32[1] == 0 && __in6_u.__u6_addr32[2] == htonl(0xffff);
}

bool Inaddr6::isV4compatible() const {
  return __in6_u.__u6_addr32[0] == 0 && __in6_u.__u6_addr32[1] == 0 && __in6_u.__u6_addr32[2] == 0 && ntohl(__in6_u.__u6_addr32[3]) > 1;
}

bool Inaddr6::operator==(const Inaddr6 &other) const {
  for (auto index = 4; index-- > 0;) {
    if (__in6_u.__u6_addr32[index] != other.__in6_u.__u6_addr32[index]) {
      return false;
    }
  }
  return true;
}

bool Inaddr6::isMulticast() const {
  return __in6_u.__u6_addr8[0] == 0xff;
}

bool SockAddr6::presentationToNetwork(const char *bindaddr) {
  addr6.clear();
  if (inet_pton(AF_INET6, bindaddr ? bindaddr : "::", &addr6) != 1) {
    errx(1, "malformed --addr argument");
    return false;
  }
  return true;
}

size_t DarkHttpd::Fd::vprintln(const char *format, va_list va) {
  FILE *stream = getStream();
  auto added = fprintf(stream, format, va);
  fputc('\n', stream);
  return ++added;
}


size_t DarkHttpd::Fd::putln(const char *content) {
  FILE *stream = getStream();
  auto added = fputs(content, stream);
  fputc('\n', stream);
  return ++added;
}

size_t DarkHttpd::Fd::printf(const char *format, ...) {
  FILE *stream = getStream();
  va_list args;
  va_start(args, format);
  auto added = vfprintf(stream, format, args);
  va_end(args);
  return added;
}

///////////////////////////
StringView::StringView(const char *pointer, size_t length, size_t start): pointer{pointer},
  length{length},
  start{start} {
  if (pointer && length == ~0) {
    this->length = strlen(&pointer[start]);
  }
}

StringView &StringView::operator=(const char *str) {
  pointer = str;
  length = strlen(str);
  start = 0;
  return *this;
}

/* Default mimetype mappings
 * //todo: use xdg-mime on systems which have it
 * //todo: only use file, with cli options to generate one from this string.
 */
static const char *default_extension_map = { //todo: move to class
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
};
//file gets read into here.

/* ---------------------------------------------------------------------------
 * Adds contents of specified file to mime_map list.
 */
void DarkHttpd::load_mime_map_file(const char *filename) {
  auto fd = open(filename, 0);
  if (fd > 0) {
    struct stat filestat;
    if (fstat(fd, &filestat) == 0) {
      if (mimeFileContent.malloc(filestat.st_size)) {
        if (read(fd, mimeFileContent, filestat.st_size) == filestat.st_size) {
          //null the whitespaces so as to make individual strings of each token.
          for (auto killer = mimeFileContent.length; killer-- > 0;) { //using index so that we can tolerate input with nulls already present between tokens
            if (strchr(" \t\r\n", mimeFileContent[killer])) { //need a sharable "isWhite"
              mimeFileContent[killer] = 0;
            }
          }
        } else {
          warn("File I/O issue loading mimetypes file, %s, content ignored", filename);
          mimeFileContent.Free();
        }
      }
    }
  }
}


StringView DarkHttpd::url_content_type(const char *url) {
  if (url != nullptr) { // useful guard, and lets us get to the default of the default.
    if (auto period = strrchr(url, '.')) {
      const char *pool = mimeFileContent ? mimeFileContent : default_extension_map;
      if (pool) { //always true unless someone deletes the built-in one.
        auto nameLength = strlen(period);
        size_t poolLength = mimeFileContent ? mimeFileContent.length : sizeof(default_extension_map);
        for (const char *searcher = &pool[poolLength - nameLength]; searcher > pool;) {
          if (*searcher == *period) { //found start of search item
            if (strncasecmp(searcher, period, nameLength) == 0) { //name match
              if (searcher[nameLength] != ':') { //not a synonym for a mime type
                while (--searcher >= pool) { //read back for a ':'
                  if (*searcher == ':') {
                    //found end of mime string
                    const char *end = searcher;
                    while (--searcher > pool) {
                      //search for white
                      //and if so the one after it to one short of searcher is our mimetype
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  /* no period found in the string or extension not found in map */
  return default_mimetype;
}

const char *DarkHttpd::get_address_text(const void *addr) const {
#ifdef HAVE_INET6
  if (inet6) {
    thread_local char text_addr[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, (const in6_addr *) addr, text_addr, INET6_ADDRSTRLEN);
    return text_addr;
  } else
#endif
  {
    return inet_ntoa(*(const in_addr *) addr);
  }
}

/* Initialize the sockin global. This is the socket that we accept
 * connections from.
 */
void DarkHttpd::init_sockin() {
  sockaddr_in addrin;
#ifdef HAVE_INET6
  SockAddr6 sock6;
#endif
  socklen_t addrin_len;
  int sockopt;

#ifdef HAVE_INET6
  if (inet6) {
    if (!sock6.presentationToNetwork(bindaddr)) {
      errx(1, "malformed --addr argument");
    }
    sockin = socket(PF_INET6, SOCK_STREAM, 0);
  } else
#endif
  {
    memset(&addrin, 0, sizeof(addrin));
    addrin.sin_addr.s_addr = bindaddr ? inet_addr(bindaddr) : INADDR_ANY;
    if (addrin.sin_addr.s_addr == (in_addr_t) INADDR_NONE) {
      errx(1, "malformed --addr argument");
    }
    sockin = socket(PF_INET, SOCK_STREAM, 0);
  }

  if (sockin == -1) {
    err(1, "socket()");
  }

  /* reuse address */
  sockopt = 1;
  if (setsockopt(sockin, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof(sockopt)) == -1) {
    err(1, "setsockopt(SO_REUSEADDR)");
  }

  /* disable Nagle since we buffer everything ourselves */
  sockopt = 1;
  if (setsockopt(sockin, IPPROTO_TCP, TCP_NODELAY, &sockopt, sizeof(sockopt)) == -1) {
    err(1, "setsockopt(TCP_NODELAY)");
  }

#ifdef HAVE_INET6
  if (inet6) {
    /* Listen on IPv4 and IPv6 on the same socket.               */
    /* Only relevant if listening on ::, but behaves normally if */
    /* listening on a specific address.                          */
    sockopt = 0;
    if (setsockopt(sockin, IPPROTO_IPV6, IPV6_V6ONLY, &sockopt, sizeof(sockopt)) < 0) {
      err(1, "setsockopt (IPV6_V6ONLY)");
    }
  }
#endif

#ifdef TORTURE
  /* torture: cripple the kernel-side send buffer so we can only squeeze out
   * one byte at a time (this is for debugging)
   */
  sockopt = 1;
  if (setsockopt(sockin, SOL_SOCKET, SO_SNDBUF, &sockopt, sizeof(sockopt)) == -1) {
    err(1, "setsockopt(SO_SNDBUF)");
  }
#endif

  /* bind socket */
#ifdef HAVE_INET6
  if (inet6) {
    sock6.sin6_family = AF_INET6;
    sock6.sin6_port = htons(bindport);
    if (bind(sockin, reinterpret_cast<sockaddr *>(&sock6), sizeof(sockaddr_in6)) == -1) {
      err(1, "bind(port %u)", bindport);
    }

    addrin_len = sizeof(sock6);
    if (getsockname(sockin, reinterpret_cast<sockaddr *>(&sock6), &addrin_len) == -1) {
      err(1, "getsockname()");
    }
    printf("listening on: http://[%s]:%u/\n", get_address_text(&sock6.sin6_addr), ntohs(sock6.sin6_port));
  } else
#endif
  {
    addrin.sin_family = PF_INET;
    addrin.sin_port = htons(bindport);
    if (bind(sockin, (sockaddr *) &addrin, sizeof(sockaddr_in)) == -1) {
      err(1, "bind(port %u)", bindport);
    }
    addrin_len = sizeof(addrin);
    if (getsockname(sockin, (sockaddr *) &addrin, &addrin_len) == -1) {
      err(1, "getsockname()");
    }
    printf("listening on: http://%s:%u/\n", get_address_text(&addrin.sin_addr), ntohs(addrin.sin_port));
  }

  /* listen on socket */
  if (listen(sockin, max_connections) == -1) {
    err(1, "listen()");
  }

  /* enable acceptfilter (this is only available on FreeBSD) */
  if (want_accf) {
#if defined(__FreeBSD__)
    struct accept_filter_arg filt = {"httpready", ""};
    if (setsockopt(sockin, SOL_SOCKET, SO_ACCEPTFILTER, &filt, sizeof(filt)) == -1) {
      fprintf(stderr, "cannot enable acceptfilter: %s\n", strerror(errno));
    } else {
      printf("enabled acceptfilter\n");
    }
#else
    printf("this platform doesn't support acceptfilter\n");
#endif
  }
}

void DarkHttpd::usage(const char *argv0) {
  printf("usage:\t%s /path/to/wwwroot [flags]\n\n", argv0);
  printf("flags:\t--port number (default: %u, or 80 if running as root)\n"
    "\t\tSpecifies which port to listen on for connections.\n"
    "\t\tPass 0 to let the system choose any free port for you.\n\n",
    bindport);
  printf("\t--addr ip (default: all)\n"
    "\t\tIf multiple interfaces are present, specifies\n"
    "\t\twhich one to bind the listening port to.\n\n");
#ifdef HAVE_INET6
  printf("\t--ipv6\n"
    "\t\tListen on IPv6 address.\n\n");
#endif
  printf("\t--daemon (default: don't daemonize)\n"
    "\t\tDetach from the controlling terminal and run in the background.\n\n");
  printf("\t--pidfile filename (default: no pidfile)\n"
    "\t\tWrite PID to the specified file. Note that if you are\n"
    "\t\tusing --chroot, then the pidfile must be relative to,\n"
    "\t\tand inside the wwwroot.\n\n");
  printf("\t--maxconn number (default: system maximum)\n"
    "\t\tSpecifies how many concurrent connections to accept.\n\n");
  printf("\t--log filename (default: stdout)\n"
    "\t\tSpecifies which file to append the request log to.\n\n");
  printf("\t--syslog\n"
    "\t\tUse syslog for request log.\n\n");
  printf("\t--index filename (default: %s)\n"
    "\t\tDefault file to serve when a directory is requested.\n\n",
    index_name);
  printf("\t--no-listing\n"
    "\t\tDo not serve listing if directory is requested.\n\n");
  printf("\t--mimetypes filename (optional)\n"
    "\t\tParses specified file for extension-MIME associations.\n\n");
  printf("\t--default-mimetype string (optional, default: %s)\n"
    "\t\tFiles with unknown extensions are served as this mimetype.\n\n", default_mimetype.pointer);
  printf("\t--uid uid/uname, --gid gid/gname (default: don't privdrop)\n"
    "\t\tDrops privileges to given uid:gid after initialization.\n\n");
  printf("\t--chroot (default: don't chroot)\n"
    "\t\tLocks server into wwwroot directory for added security.\n\n");
#ifdef __FreeBSD__
  printf("\t--accf (default: don't use acceptfilter)\n"
         "\t\tUse acceptfilter. Needs the accf_http kernel module loaded.\n\n");
#endif
  printf("\t--no-keepalive\n"
    "\t\tDisables HTTP Keep-Alive functionality.\n\n");
  printf("\t--single-file\n"
    "\t\tOnly serve a single file provided as /path/to/file instead\n"
    "\t\tof a whole directory.\n\n");
  printf("\t--forward host url (default: don't forward)\n"
    "\t\tWeb forward (301 redirect).\n"
    "\t\tRequests to the host are redirected to the corresponding url.\n"
    "\t\tThe option may be specified multiple times, in which case\n"
    "\t\tthe host is matched in order of appearance.\n\n");
  printf("\t--forward-all url (default: don't forward)\n"
    "\t\tWeb forward (301 redirect).\n"
    "\t\tAll requests are redirected to the corresponding url.\n\n");
  printf("\t--forward-https\n"
    "\t\tIf the client requested HTTP, forward to HTTPS.\n"
    "\t\tThis is useful if darkhttpd is behind a reverse proxy\n"
    "\t\tthat supports SSL.\n\n");
  printf("\t--no-server-id\n"
    "\t\tDon't identify the server type in headers\n"
    "\t\tor directory listings.\n\n");
  printf("\t--timeout secs (default: %d)\n"
    "\t\tIf a connection is idle for more than this many seconds,\n"
    "\t\tit will be closed. Set to zero to disable timeouts.\n\n",
    timeout_secs);
  printf("\t--auth username:password\n"
    "\t\tEnable basic authentication. This is *INSECURE*: passwords\n"
    "\t\tare sent unencrypted over HTTP, plus the password is visible\n"
    "\t\tin ps(1) to other users on the system.\n\n");
  printf("\t--header 'Header: Value'\n"
    "\t\tAdd a custom header to all responses.\n"
    "\t\tThis option can be specified multiple times, in which case\n"
    "\t\tthe headers are added in order of appearance.\n\n");
#ifndef HAVE_INET6
  printf("\t(This binary was built without IPv6 support: -DNO_IPV6)\n\n");
#endif
}

static unsigned char base64_mapped(unsigned char low6bits) {
#if 1 // less code bytes
  low6bits &= 63; // do this here instead of at all points of use.
  return low6bits + (low6bits < 26 ?
                       'A' :
                       low6bits < 52 ?
                         'a' - 26 :
                         low6bits < 62 ?
                           '0' - 52 :
                           (low6bits == 62 ? '+' : '/') - low6bits);
#else // easier to read
  const char *base64_table = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                             "abcdefghijklmnopqrstuvwxyz"
                             "0123456789"
                             "+/"; // reformatter kept on trashing the array so I made it into strings.
  return base64_table[0x3F & low6bits];
#endif
}

// bug-fixed: DarkHttpd's base 64 encoder output excess chars at end of string when padding is needed.
static unsigned char *base64_encode(char *str) {
  unsigned input_length = strlen(str);
  unsigned output_length = 4 * ((input_length + 2) / 3);

  auto encoded_data = static_cast<unsigned char *>(malloc(output_length + 1));
  if (encoded_data == nullptr) {
    return nullptr;
  }
  auto writer = encoded_data;

  for (int i = 0; i < input_length;) {
    unsigned char first = *str++; // i already tested by 'for'
    *writer++ = base64_mapped(first >> 2); // top 6 leaves 2 behind
    if (i < input_length) {
      unsigned char second = *str++;
      *writer++ = base64_mapped(first << 4 | second >> 4); // 2 to high nibble and 4 from new to low
      if (i < input_length) {
        unsigned char third = *str++;
        *writer++ = base64_mapped(second << 2 | third >> 6); // 4 low become high of 6, need 2 from next one
        *writer++ = base64_mapped(third); // final 6
      } else {
        *writer++ = '='; // pad after 2nd
      }
    } else {
      *writer++ = base64_mapped(first << 4); // 2 to high nibble then two padding
      // prior implementation output one extra char here., 4 for every 3 and then the pads.
      *writer++ = '=';
      *writer++ = '=';
    }
  }
  *writer = 0;
  return encoded_data;
}

/* @returns whether string is strictly a number.  Set num to NULL if disinterested in its value.
 */
static bool str_to_num(const char *str, long long *num) {
  char *endptr;
  long long n;

  errno = 0;
  n = strtoll(str, &endptr, 10);
  if (*endptr != 0) {
    return false; //todo:1 check for KMG multiplier.
  }
  if (n == LLONG_MIN && errno == ERANGE) {
    return false;
  }
  if (n == LLONG_MAX && errno == ERANGE) {
    return false;
  }
  if (num != nullptr) {
    *num = n;
  }
  return true;
}

/* @returns a valid number or dies.
 * @deprecated
 */
static long long xstr_to_num(const char *str) {
  long long ret;

  if (!str_to_num(str, &ret)) {
    errx(1, "number \"%s\" is invalid", str);
  }
  return ret;
}

bool DarkHttpd::parse_commandline(int argc, char *argv[]) {
  if (argc < 2 || (argc == 2 && strcmp(argv[1], "--help") == 0)) {
    usage(argv[0]); /* no wwwroot given */
    // exit(EXIT_SUCCESS);
    return false;
  }
  bindport = getuid() ? 8080 : 80; // default if run as root.
  custom_hdrs.clear();

  wwwroot = argv[1];
  /* Strip ending slash.
   * todo: should use the url cleaner to deal with . and .. in the string */
  if (wwwroot.length == 0) {
    errx(1, "/path/to/wwwroot cannot be empty");
    return false;
  }

  wwwroot.trimTrailing("/"); //former code only trimmed a single trailing slash, 980f trims multiple.

  /* walk through the remainder of the arguments (if any) */
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--port") == 0) {
      if (++i >= argc) {
        errx(1, "missing number after --port");
        return false;
      }
      bindport = xstr_to_num(argv[i]);
    } else if (strcmp(argv[i], "--addr") == 0) {
      if (++i >= argc) {
        errx(1, "missing ip after --addr");
        return false;
      }
      bindaddr = argv[i];
    } else if (strcmp(argv[i], "--maxconn") == 0) {
      if (++i >= argc) {
        errx(1, "missing number after --maxconn");
        return false;
      }
      max_connections = (int) xstr_to_num(argv[i]);
    } else if (strcmp(argv[i], "--log") == 0) {
      if (++i >= argc) {
        errx(1, "missing filename after --log");
        return false;
      }
      logfile_name = argv[i];
    } else if (strcmp(argv[i], "--chroot") == 0) {
      want_chroot = true;
    } else if (strcmp(argv[i], "--daemon") == 0) {
      want_daemon = true;
    } else if (strcmp(argv[i], "--index") == 0) {
      if (++i >= argc) {
        errx(1, "missing filename after --index");
        return false;
      }
      index_name = argv[i];
    } else if (strcmp(argv[i], "--no-listing") == 0) {
      no_listing = true;
    } else if (strcmp(argv[i], "--mimetypes") == 0) {
      if (++i >= argc) {
        errx(1, "missing filename after --mimetypes");
        return false;
      }
      load_mime_map_file(argv[i]);
    } else if (strcmp(argv[i], "--default-mimetype") == 0) {
      if (++i >= argc) {
        errx(1, "missing string after --default-mimetype");
        return false;
      }
      default_mimetype = argv[i];
    } else if (strcmp(argv[i], "--uid") == 0) {
      if (++i >= argc) {
        errx(1, "missing uid after --uid");
        return false;
      }
      passwd *p = getpwnam(argv[i]);
      if (!p) {
        p = getpwuid((uid_t) xstr_to_num(argv[i]));
      }
      if (!p) {
        errx(1, "no such uid: `%s'", argv[i]);
        return false;
      }
      drop_uid = p->pw_uid;
    } else if (strcmp(argv[i], "--gid") == 0) {
      if (++i >= argc) {
        errx(1, "missing gid after --gid");
        return false;
      }
      group *g = getgrnam(argv[i]);
      if (!g) {
        g = getgrgid((gid_t) xstr_to_num(argv[i]));
      }
      if (!g) {
        errx(1, "no such gid: `%s'", argv[i]);
      }
      drop_gid = g->gr_gid;
    } else if (strcmp(argv[i], "--pidfile") == 0) {
      if (++i >= argc) {
        errx(1, "missing filename after --pidfile");
      }
      pid.file_name = argv[i];
    } else if (strcmp(argv[i], "--no-keepalive") == 0) {
      want_keepalive = false;
    } else if (strcmp(argv[i], "--accf") == 0) {
      want_accf = true;
    } else if (strcmp(argv[i], "--syslog") == 0) {
      syslog_enabled = true;
    } else if (strcmp(argv[i], "--single-file") == 0) {
      want_single_file = true;
    } else if (strcmp(argv[i], "--forward") == 0) {
      if (++i >= argc) {
        errx(1, "missing host after --forward");
      }
      const char *host = argv[i];
      if (++i >= argc) {
        errx(1, "missing url after --forward");
      }
      const char *url = argv[i];
      forward_map.add(host, url);
    } else if (strcmp(argv[i], "--forward-all") == 0) {
      if (++i >= argc) {
        errx(1, "missing url after --forward-all");
      }
      forward_all_url = argv[i];
    } else if (strcmp(argv[i], "--no-server-id") == 0) {
      want_server_id = false;
    } else if (strcmp(argv[i], "--timeout") == 0) {
      if (++i >= argc) {
        errx(1, "missing number after --timeout");
      }
      timeout_secs = (int) xstr_to_num(argv[i]);
    } else if (strcmp(argv[i], "--auth") == 0) {
      if (++i >= argc || strchr(argv[i], ':') == nullptr) {
        errx(1, "missing 'user:pass' after --auth");
      }

      AutoString key = reinterpret_cast<char *>(base64_encode(argv[i]));
      xasprintf(auth_key, "Basic %s", key.pointer);
    } else if (strcmp(argv[i], "--forward-https") == 0) {
      forward_to_https = true;
    } else if (strcmp(argv[i], "--header") == 0) {
      if (++i >= argc) {
        errx(1, "missing argument after --header");
      }
      if (strchr(argv[i], '\n') != nullptr || strstr(argv[i], ": ") == nullptr) {
        errx(1, "malformed argument after --header");
      }
      //the following is not efficient, what we really need is a list of strings that we cat together in the stream output.
      custom_hdrs.push_back(argv[i]);
    }
#ifdef HAVE_INET6
    else if (strcmp(argv[i], "--ipv6") == 0) {
      inet6 = true;
    }
#endif
    else {
      errx(1, "unknown argument `%s'", argv[i]);
      return false;
    }
  }
  return true;
}


/* Accept a connection from sockin and add it to the connection queue. */
void DarkHttpd::accept_connection() {
  sockaddr_in addrin;
#ifdef HAVE_INET6
  sockaddr_in6 addrin6;
#endif
  socklen_t sin_size;
  Connection *conn;
  int fd;

#ifdef HAVE_INET6
  if (inet6) {
    sin_size = sizeof(addrin6);
    memset(&addrin6, 0, sin_size);
    fd = accept(sockin, (sockaddr *) &addrin6, &sin_size);
  } else
#endif
  {
    sin_size = sizeof(addrin);
    memset(&addrin, 0, sin_size);
    fd = accept(sockin, (sockaddr *) &addrin, &sin_size);
  }

  if (fd == -1) {
    /* Failed to accept, but try to keep serving existing connections. */
    if (errno == EMFILE || errno == ENFILE) {
      accepting = false;
    }
    warn("accept()");
    return;
  }

  /* Allocate and initialize struct connection. */
  conn = new Connection(*this,fd); // connections have defaults from the DarkHttpd that creates them.

#ifdef HAVE_INET6
  if (inet6) {
    conn->client = addrin6.sin6_addr;
  } else
#endif
  {
    *reinterpret_cast<in_addr_t *>(&conn->client) = addrin.sin_addr.s_addr;
  }
  entries.push_front(conn);

  debug("accepted connection from %s:%u (fd %d)\n", inet_ntoa(addrin.sin_addr), ntohs(addrin.sin_port), int(conn->socket));

  /* Try to read straight away rather than going through another iteration
   * of the select() loop.
   */
  conn->poll_recv_request();
}

/* Should this character be logencoded?
 */
static bool needs_logencoding(const unsigned char c) {
  return ((c <= 0x1F) || (c >= 0x7F) || (c == '"'));
}

/* Encode string for logging.
 */
static void logencode(const char *src, char *dest) {
  static const char hex[] = "0123456789ABCDEF";
  int i, j;

  for (i = j = 0; src[i] != '\0'; i++) {
    if (needs_logencoding((unsigned char) src[i])) {
      dest[j++] = '%';
      dest[j++] = hex[(src[i] >> 4) & 0xF];
      dest[j++] = hex[src[i] & 0xF];
    } else {
      dest[j++] = src[i];
    }
  }
  dest[j] = '\0';
}

/* Format [when] as a CLF date format, stored in the specified buffer.  The same
 * buffer is returned for convenience.
 */
#define CLF_DATE_LEN 29 /* strlen("[10/Oct/2000:13:55:36 -0700]")+1 */

static char *clf_date(char *dest, time_t when) {
  tm tm;
  localtime_r(&when, &tm);
  if (strftime(dest, CLF_DATE_LEN, "[%d/%b/%Y:%H:%M:%S %z]", &tm) == 0) {
    dest[0] = 0;
  }
  return dest;
}

/* Add a connection's details to the logfile. */
void DarkHttpd::log_connection(const Connection *conn) {
  AutoString safe_method;
  AutoString safe_url;
  AutoString safe_referer;
  AutoString safe_user_agent;
  char dest[CLF_DATE_LEN];

  if (logfile == nullptr) {
    return;
  }
  if (conn->http_code == 0) {
    return; /* invalid - died in request */
  }
  if (!conn->method) {
    return; /* invalid - didn't parse - maybe too long */
  }

  // all the _safe macros can go away if we stream with a encoding translator in the stream instead of this malloc and free and explode methodology.
#define make_safe(x)                                                    \
  do {                                                                  \
    if (conn->x) {                                                      \
      safe_## x = static_cast<char *>(xmalloc(strlen(conn->x) * 3 + 1)); \
      logencode(conn->x, safe_## x);                                     \
    } else {                                                            \
      safe_## x = NULL;                                                  \
    }                                                                   \
  } while (0)

  make_safe(method);
  make_safe(url);
  make_safe(referer);
  make_safe(user_agent);

#undef make_safe

#define use_safe(x) safe_## x ? safe_## x.pointer : ""
  if (syslog_enabled) {
    syslog(LOG_INFO, "%s - - %s \"%s %s HTTP/1.1\" %d %llu \"%s\" \"%s\"\n",
      get_address_text(&conn->client),
      clf_date(dest, now),
      use_safe(method),
      use_safe(url),
      conn->http_code,
      llu(conn->total_sent),
      use_safe(referer),
      use_safe(user_agent));
  } else {
    fprintf(logfile, "%s - - %s \"%s %s HTTP/1.1\" %d %llu \"%s\" \"%s\"\n",
      get_address_text(&conn->client),
      clf_date(dest, now),
      use_safe(method),
      use_safe(url),
      conn->http_code,
      llu(conn->total_sent),
      use_safe(referer),
      use_safe(user_agent));
    fflush(logfile);
  }

#undef use_safe
}

DarkHttpd::Connection::Connection(DarkHttpd &parent, int fd): service(parent),socket(fd) {
  memset(&client, 0, sizeof(client));
  nonblock_socket(socket);
  state = RECV_REQUEST;
  last_active = service.now;
}
/* Log a connection, then cleanly deallocate its internals. */
void DarkHttpd::Connection::clear() {
  request = nullptr;
  method = nullptr;
  url = nullptr;
  referer = nullptr;
  user_agent = nullptr;
  authorization = nullptr;
  if (!header_dont_free) {
    header = nullptr;
  }
  if (!reply_dont_free) {
    reply = nullptr;
  }
}

/* Recycle a finished connection for HTTP/1.1 Keep-Alive. */
void DarkHttpd::Connection::recycle() {
  clear(); //legacy, separate heap usage clear from the rest.
  debug("free_connection(%d)\n", int(socket));
  xclose(socket);
  range_begin = 0;
  range_end = 0;
  range_begin_given = false;
  range_end_given = false;

  header_dont_free = false;
  reply_dont_free = false;

  header_sent = 0;
  header_only = false;
  http_code = 0;
  conn_closed = true;
  reply_fd.forget(); // but it might be still open ?!
  reply_start = 0;
  file_length = 0;
  reply_sent = 0;
  total_sent = 0;

  state = RECV_REQUEST; /* ready for another */
}

/* If a connection has been idle for more than timeout_secs, it will be
 * marked as DONE and killed off in httpd_poll().
 */
void DarkHttpd::Connection::poll_check_timeout() {
  if (service.timeout_secs > 0) {
    if (service.now - last_active >= service.timeout_secs) {
      debug("poll_check_timeout(%d) marking connection closed\n", int(socket));
      conn_closed = true;
      state = DONE;
    }
  }
}

/* Format [when] as an RFC1123 date, stored in the specified buffer.  The same
 * buffer is returned for convenience.
 */
#define DATE_LEN 30 /* strlen("Fri, 28 Feb 2003 00:02:08 GMT")+1 */

static char *rfc1123_date(char *dest, time_t when) {
  dest[strftime(dest, DATE_LEN, "%a, %d %b %Y %H:%M:%S GMT", gmtime(&when))] = 0; // strftime returns number of chars it put into dest.
  return dest;
}

static char HEX_TO_DIGIT(char hex) {
  if (hex >= 'A' && hex <= 'F') {
    return hex - 'A' + 10;
  }
  if (hex >= 'a' && hex <= 'f') {
    return hex - 'a' + 10;
  }
  return hex - '0';
}


/* Decode URL by converting %XX (where XX are hexadecimal digits) to the
 * character it represents.  Don't forget to free the return value.
 */
static char *urldecode(const char *url) {
  size_t len = strlen(url);
  char *out = static_cast<char *>(xmalloc(len + 1));
  char *writer = out;

  while (char c = *url++) {
    if (c == '%' && url[1] && isxdigit(url[1]) // because we have already used strlen we know there is a null char we can rely upon here
        && url[2] && isxdigit(url[2])) {
      *writer++ = HEX_TO_DIGIT(*url++) << 4 + HEX_TO_DIGIT(*url++);
      continue;
    }
    *writer++ = c; /* straight copy */
  }

  *writer = 0;
  return out;
}

/* Returns Connection or Keep-Alive header, depending on conn_closed. */
const char *DarkHttpd::Connection::keep_alive() const {
  return conn_closed ? "Connection: close\r\n" : static_cast<const char *>(service.keep_alive_field);
}

/* "Generated by " + pkgname + " on " + date + "\n"
 *  1234567890123               1234            2 ('\n' and '\0')
 */
static char _generated_on_buf[13 + sizeof(pkgname) - 1 + 4 + DATE_LEN + 2];

const char *DarkHttpd::generated_on(const char date[DATE_LEN]) const {
  if (!want_server_id) {
    return "";
  }
  snprintf(_generated_on_buf, sizeof(_generated_on_buf), "Generated by %s on %s\n", pkgname, date);
  return _generated_on_buf;
}


void DarkHttpd::Connection::startHeader(const int errcode, const char *errtext) {
  header_fd.printf("HTTP/1.1 %d %s\r\n", http_code = errcode, errtext);
}

void DarkHttpd::Connection::catDate() {
  header_fd.printf("Date: %s\r\n", service.now.image);
}

void DarkHttpd::Connection::catServer() {
  if (service.want_server_id) {
    header_fd.printf("Server: %s\r\n", pkgname);
  }
}

void DarkHttpd::Connection::catFixed(const char *fixedText) {
  header_fd.printf(fixedText);
}

void DarkHttpd::Connection::catKeepAlive() {
  if (conn_closed) {
    header_fd.printf("Connection: close\r\n");
  } else {
    header_fd.printf("Keep-Alive: timeout=%d\r\n", service.timeout_secs);
  }
}

void DarkHttpd::Connection::catCustomHeaders() {
  for (auto custom_Hdr: service.custom_hdrs) {
    header_fd.printf("%s\r\n", custom_Hdr);
  }
}

void DarkHttpd::Connection::catContentLength(off_t off) {
  header_fd.printf("Content-Length: %llu\r\n", llu(off));
}

void DarkHttpd::Connection::startCommonHeader(int errcode, const char *errtext, off_t contentLenght=~0UL) {
  startHeader( errcode,errtext);
  catDate();
  catServer();
  catFixed("Accept-Ranges: bytes\r\n");
  catKeepAlive();
  catCustomHeaders();
  if (~contentLenght!=0) {
    catContentLength(contentLenght);
  }
}


void DarkHttpd::Connection::catAuth() {
  if (service.auth_key) {
    header_fd.printf( //AI: "Authorization: Basic %s\r\n", service.auth_key); //leak our key to the world!!
      "WWW-Authenticate: Basic realm=\"User Visible Realm\"\r\n");
  }
}

void DarkHttpd::Connection::catGeneratedOn(bool toReply) {
  if (service.want_server_id) {
    (toReply ? reply_fd : header_fd).printf("Generated by %s on %s\n", pkgname, service.now.image);
  }
}


void DarkHttpd::Connection::endHeader() {
  header_fd.printf("\r\n");
}

void DarkHttpd::Connection::startReply(int errcode, const char *errtext) {
  reply_fd.printf("<!DOCTYPE html><html><head><title>%d %s</title></head><body>\n" "<h1>%s</h1>\n", errcode, errtext, errtext);
}

void DarkHttpd::Connection::addFooter() {
  reply_fd.putln("<hr>");
  catGeneratedOn(true);
  reply_fd.putln("</body></html>");
}

/* A default reply for any (erroneous) occasion. */
void DarkHttpd::Connection::default_reply(const int errcode, const char *errname, const char *format, ...) {
  startReply(errcode, errname);
  va_list va;
  va_start(va, format);
  reply_fd.vprintln(format, va);
  va_end(va);
  addFooter();

  //todo:file_length is not related to the file length!
  startCommonHeader(errcode, errname,file_length);
  catFixed("Content-Type: text/html; charset=UTF-8\r\n"); //todo: use catMime();
  catAuth();
  endHeader();

  reply_type = REPLY_GENERATED;
  /* Reset reply_start in case the request set a range. */
  reply_start = 0;
}

void DarkHttpd::Connection::redirect(const char *format, ...) {
  AutoString where;

  va_list va;
  va_start(va, format);
  xvasprintf(where, format, va);
  va_end(va);

  startReply(http_code = 301, "Moved Permanently");
  reply_fd.printf("Moved to: <a href=\"%s\">%s</a>\n", where.pointer, where.pointer); /* where x 2 */
  addFooter();

  startHeader(301, "Moved Permanently");
  reply_fd.printf("Date: %s\r\n", service.now.image);
  catServer();

  /* "Accept-Ranges: bytes\r\n" - not relevant here */
  reply_fd.printf("Location: %s\r\n", where.pointer);
  catKeepAlive();
  catCustomHeaders();
  catContentLength(file_length);//todo:suspicious, iseither 0 or stale.
  catFixed("Content-Type: text/html; charset=UTF-8\r\n"); //todo: use catMime();
  //no auth?
  endHeader();
  reply_type = REPLY_GENERATED;
}

/* Parses a single HTTP request field.  Returns string from end of [field] to
 * first \r, \n or end of request string.  Returns NULL if [field] can't be
 * matched.  Case insensitive.
 *
 * You need to remember to deallocate the result.
 * example: parse_field(conn, "Referer: ");
 */
char *DarkHttpd::Connection::parse_field(const char *field) const {
  /* find start */
  char *pos = strcasestr(request, field);
  if (pos == nullptr) {
    return nullptr;
  }
  pos += strlen(field); //char just past end of 'field'
  /* find end of line */
  auto eol = strpbrk(pos, "\r\n");
  if (!eol) { //already null terminated, just dup it
    return strdup(pos);
  }
  return sub_string(pos, eol);
}

void DarkHttpd::Connection::redirect_https() {
  /* work out path of file being requested */
  AutoString url(urldecode(url));

  /* make sure it's safe */
  if (make_safe_url(url) == nullptr) {
    default_reply(400, "Bad Request", "You requested an invalid URL.");
    return;
  }

  AutoString host(parse_field("Host: "));
  if (host == nullptr) {
    default_reply(400, "Bad Request",
      "Missing 'Host' header.");
    return;
  }
  redirect("https://%s%s", host.pointer, url.pointer);
}

bool DarkHttpd::Connection::is_https_redirect() const {
  if (service.forward_to_https == false) {
    return false; /* --forward-https was never used */
  }

  AutoString proto(parse_field("X-Forwarded-Proto: "));
  if (proto == nullptr || strcasecmp(proto, "https") == 0) {
    return false;
  }
  return true;
}

/* Parse a Range: field into range_begin and range_end.  Only handles the
 * first range if a list is given.  Sets range_{begin,end}_given to true if
 * associated part of the range is given.
 * "Range: bytes=500-999"  is all that was supported, changing to also accept:
 * "Range: - 456 last 456
 * "Range: 789 -
 * todo: strtok walk with "=-,\r\n"
 */
void DarkHttpd::Connection::parse_range_field() {
  AutoString range(parse_field("Range: bytes="));
  if (!range) {
    return;
  }
  range_end_given = range_begin_given = false; //ensure erased any prior attempt
  char *end = nullptr;
  range_begin = strtoll(range, &end, 10);
  range_begin_given = end > range; //and we already parsed it
  /* parse number up to hyphen */
  if (range_begin < 0) { // Range -456, no white space after
    //offset is from end of file
  }
  if (*end == '-') {
    range_end = strtoll(++end, &end, 10);
    if (*end != ',') {
      range_end = 0; //forget old as well as reject new
      return; /* must be end of string or a list to be valid */
    }
    range_end_given = range_end != 0;
    return; /* we are done, "from here to end of file" is being requested */
  }

  //behaves differently on case "2134234-0," where it should give an closed and invalid range but it instead gives an end of 0
}

/* Parse an HTTP request like "GET / HTTP/1.1" to get the method (GET), the
 * url (/), the referer (if given) and the user-agent (if given).  Remember to
 * deallocate all these buffers.  The method will be returned in uppercase.
 */
bool DarkHttpd::Connection::parse_request() {
  assert(request.notTrivial());

  /* parse method */
  auto firstspace = strchr(request, ' ');

  method = sub_string(request, firstspace);
  method.toUpper();

  /* parse url */
  const char *nextspace = strchr(++firstspace, ' ');

  if (!nextspace) {
    return false; /* fail */
  }
  const char *end = strpbrk(nextspace, " \r\n");
  url = sub_string(nextspace, end);

  /* parse protocol to determine conn_closed */
  if (*end == ' ') {
    nextspace = removeLeadingWhitespace(end);
    end = strpbrk(nextspace, " \r\n"); //old code insisted on \r, without safety of \n

    AutoString proto(sub_string(nextspace, end));
    if (strcasecmp(proto, "HTTP/1.1") == 0) {
      conn_closed = false;
    }
  }

  /* parse connection field */
  AutoString tmp(parse_field("Connection: "));
  if (tmp) {
    if (strcasecmp(tmp, "close") == 0) {
      conn_closed = true;
    } else if (strcasecmp(tmp, "keep-alive") == 0) {
      conn_closed = false;
    }
  }

  /* cmdline flag can be used to deny keep-alive */
  if (!service.want_keepalive) {
    conn_closed = true;
  }

  /* parse important fields */
  referer = parse_field("Referer: ");
  user_agent = parse_field("User-Agent: ");
  authorization = parse_field("Authorization: ");
  parse_range_field();
  return 1;
}

static bool file_exists(const char *path) {
  struct stat filestat;
  return stat(path, &filestat) == -1 && errno == ENOENT ? false : true;
}

struct dlent {
  char *name; /* The name/path of the entry.                 */
  int is_dir; /* If the entry is a directory and not a file. */
  off_t size; /* The size of the entry, in bytes.            */
  timespec mtime; /* When the file was last modified.            */
};

static int dlent_cmp(const void *a, const void *b) {
  return strcmp((*static_cast<const dlent *const *>(a))->name, (*static_cast<const dlent *const *>(b))->name);
}

/* Make sorted list of files in a directory.  Returns number of entries, or -1
 * if error occurs.
 */
static ssize_t make_sorted_dirlist(const char *path, dlent ***output) {
  DIR *dir;
  dirent *ent;
  size_t entries = 0;
  size_t pool = 128;

  dir = opendir(path);
  if (dir == nullptr) {
    return -1;
  }

  AutoString currname(static_cast<char *>(xmalloc(strlen(path) + MAXNAMLEN + 1)));
  dlent **list = static_cast<struct dlent **>(xmalloc(sizeof(struct dlent *) * pool));

  /* construct list */
  while ((ent = readdir(dir)) != nullptr) {
    struct stat s;

    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue; /* skip "." and ".." */
    }
    assert(strlen(ent->d_name) <= MAXNAMLEN);
    sprintf(currname, "%s%s", path, ent->d_name);
    if (stat(currname, &s) == -1) {
      continue; /* skip un-stat-able files */
    }
    if (entries == pool) {
      pool *= 2;
      list = static_cast<dlent **>(xrealloc(list, sizeof(dlent *) * pool));
    }
    list[entries] = static_cast<dlent *>(xmalloc(sizeof(dlent)));
    list[entries]->name = xstrdup(ent->d_name);
    list[entries]->is_dir = S_ISDIR(s.st_mode);
    list[entries]->size = s.st_size;
    list[entries]->mtime = s.st_mtim;
    entries++;
  }
  closedir(dir);
  qsort(list, entries, sizeof(dlent *), dlent_cmp);
  *output = list;
  return (ssize_t) entries;
}

/* Cleanly deallocate a sorted list of directory files. */
static void cleanup_sorted_dirlist(dlent **list, const ssize_t size) {
  for (ssize_t i = 0; i < size; i++) {
    free(list[i]->name);
    free(list[i]);
  }
}

/* Is this an unreserved character according to
 * https://tools.ietf.org/html/rfc3986#section-2.3
 */
static bool is_unreserved(const unsigned char c) {
  if (c >= 'a' && c <= 'z') {
    return true;
  }
  if (c >= 'A' && c <= 'Z') {
    return true;
  }
  if (c >= '0' && c <= '9') {
    return true;
  }
  switch (c) {
    case '-':
    case '.':
    case '_':
    case '~':
      return true;
  }
  return false;
}

/* Encode string to be an RFC3986-compliant URL part.
 * Contributed by nf.
 */
static void urlencode(const char *src, char *dest) {
  static const char hex[] = "0123456789ABCDEF";

  while (char c = *src++) {
    if (!is_unreserved(c)) {
      *dest++ = '%';
      *dest++ = hex[0xF & c >> 4];
      *dest++ = hex[0xF & c];
    } else {
      *dest++ = c;
    }
  }
  *dest++ = '\0';
}

/* Escape < > & ' " into HTML entities. */
static void append_escaped(apbuf *dst, const char *src) {
  int pos = 0;
  while (src[pos] != '\0') {
    switch (src[pos]) {
      case '<':
        append(dst, "&lt;");
        break;
      case '>':
        append(dst, "&gt;");
        break;
      case '&':
        append(dst, "&amp;");
        break;
      case '\'':
        append(dst, "&apos;");
        break;
      case '"':
        append(dst, "&quot;");
        break;
      default:
        dst->appendl(src + pos, 1);
    }
    pos++;
  }
}

void DarkHttpd::Connection::generate_dir_listing(  const char *path, const char *decoded_url) {
  size_t maxlen = 3; /* There has to be ".." */

  dlent **list;
  ssize_t listsize = make_sorted_dirlist(path, &list);
  if (listsize == -1) {
    /* opendir() failed */
    if (errno == EACCES) {
      default_reply(403, "Forbidden", "You don't have permission to access this URL.");
    } else if (errno == ENOENT) {
      default_reply(404, "Not Found", "The URL you requested was not found.");
    } else {
      default_reply(500, "Internal Server Error", "Couldn't list directory: %s", strerror(errno));
    }
    return;
  }

  for (int i = 0; i < listsize; i++) {
    size_t tmp = strlen(list[i]->name);
    if (list[i]->is_dir) {
      tmp++; /* add 1 for '/' */
    }
    if (maxlen < tmp) {
      maxlen = tmp;
    }
  }

  apbuf *listing = new apbuf();
  append(listing, "<!DOCTYPE html>\n<html>\n<head>\n<title>");
  append_escaped(listing, decoded_url);
  append(listing,
    "</title>\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "</head>\n<body>\n<h1>");
  append_escaped(listing, decoded_url);
  append(listing, "</h1>\n<pre>\n");

  AutoString spaces = static_cast<char *>(xmalloc(maxlen));
  memset(spaces.pointer, ' ', maxlen);

  /* append ".." entry if not in wwwroot */
  if (strncmp(path, service.wwwroot.begin(), std::min((strlen(path) - 1), service.wwwroot.length)) != 0) {
    append(listing, "<a href=\"../\">..</a>/\n");
  }

  for (int i = 0; i < listsize; i++) {
    /* If a filename is made up of entirely unsafe chars,
     * the url would be three times its original length.
     */
    char safe_url[MAXNAMLEN * 3 + 1];

    urlencode(list[i]->name, safe_url);

    append(listing, "<a href=\"");
    append(listing, safe_url);
    if (list[i]->is_dir) {
      append(listing, "/");
    }
    append(listing, "\">");
    append_escaped(listing, list[i]->name);
    append(listing, "</a>");

    char buf[DIR_LIST_MTIME_SIZE];
    tm tm;
    localtime_r(&list[i]->mtime.tv_sec, &tm);
    strftime(buf, sizeof buf, DIR_LIST_MTIME_FORMAT, &tm);

    if (list[i]->is_dir) {
      append(listing, "/");
      listing->appendl(spaces, maxlen - strlen(list[i]->name));
      append(listing, buf);
      append(listing, "\n");
    } else {
      listing->appendl(spaces, maxlen - strlen(list[i]->name));
      append(listing, " ");
      append(listing, buf);
      listing->appendf(" %10llu\n", llu(list[i]->size));
    }
  }

  cleanup_sorted_dirlist(list, listsize);
  free(list);

  append(listing, "</pre>\n<hr>\n");

  append(listing, service.generated_on(service.now.image));
  append(listing, "</body>\n</html>\n");

  reply = listing->str;
  // conn.reply_length = (off_t) listing->length;
  free(listing); /* don't free inside of listing */

  startCommonHeader(200,"OK",reply.length);

  catFixed("Content-Type: text/html; charset=UTF-8\r\n");
  endHeader();
  reply_type = REPLY_GENERATED;

}

/* Process a GET/HEAD request. */
void DarkHttpd::Connection::process_get() {
  // char *end;
  char date[DATE_LEN];
  char lastmod[DATE_LEN];

  const char *forward_to = nullptr;

  /* strip out query params */
  auto end = strchr(url, '?');
  if (end != nullptr) {
    *end = '\0'; //modifies url!
  }

  /* work out path of file being requested */
  AutoString decoded_url(urldecode(url));

  /* make sure it's safe */
  if (make_safe_url(decoded_url) == nullptr) {
    default_reply(400, "Bad Request", "You requested an invalid URL.");
    return;
  }

  /* test the host against web forward options */
  if (service.forward_map.size() > 0) {
    AutoString host(parse_field("Host: "));
    if (host) {
      debug("host=\"%s\"\n", host.pointer);
      forward_to = service.forward_map.at(host);
    }
  }
  if (!forward_to) {
    forward_to = service.forward_all_url;
  }
  if (forward_to) {
    redirect("%s%s", forward_to, decoded_url.pointer);
    return;
  }

  StringView mimetype;
  AutoString target;
  if (service.want_single_file) {
    target = service.wwwroot.clone();
    mimetype = service.url_content_type(target);
  } else if (decoded_url.endsWith('/')) {
    /* does it end in a slash? serve up url/index_name */
    xasprintf(target, "%s%s%s", service.wwwroot.pointer, decoded_url.pointer, service.index_name);
    if (!file_exists(target)) {
      if (service.no_listing) {
        /* Return 404 instead of 403 to make --no-listing
         * indistinguishable from the directory not existing.
         * i.e.: Don't leak information.
         */
        default_reply(404, "Not Found", "The URL you requested was not found.");
        return;
      }
      xasprintf(target, "%s%s", service.wwwroot.pointer, decoded_url.pointer);
      generate_dir_listing(target, decoded_url);
      return;
    }
    mimetype = service.url_content_type(service.index_name);
  } else {
    /* points to a file */
    xasprintf(target, "%s%s", service.wwwroot.pointer, decoded_url.pointer);
    mimetype = service.url_content_type(decoded_url);
  }

  debug("url=\"%s\", target=\"%s\", content-type=\"%s\"\n", url.pointer, target.pointer, mimetype.pointer);

  /* open file */
  reply_fd = open(target, O_RDONLY | O_NONBLOCK);

  if (!reply_fd) {
    /* open() failed */
    if (errno == EACCES) {
      default_reply(403, "Forbidden", "You don't have permission to access this URL.");
    } else if (errno == ENOENT) {
      default_reply(404, "Not Found", "The URL you requested was not found.");
    } else {
      default_reply(500, "Internal Server Error", "The URL you requested cannot be returned: %s.", strerror(errno));
    }

    return;
  }

  /* stat the file */
  struct stat filestat;
  if (fstat(reply_fd, &filestat) == -1) {
    default_reply(500, "Internal Server Error", "fstat() failed: %s.", strerror(errno));
    return;
  }

  /* make sure it's a regular file */
  if ((S_ISDIR(filestat.st_mode)) && (!service.want_single_file)) {
    redirect("%s/", url.pointer);
    return;
  } else if (!S_ISREG(filestat.st_mode)) {
    default_reply(403, "Forbidden", "Not a regular file.");
    return;
  }

  reply_type = REPLY_FROMFILE;
  rfc1123_date(lastmod, filestat.st_mtime);

  /* check for If-Modified-Since, may not have to send */
  AutoString if_mod_since(parse_field("If-Modified-Since: "));

  if ((if_mod_since) && (strcmp(if_mod_since, lastmod) == 0)) {
    debug("not modified since %s\n", if_mod_since.pointer);
    startCommonHeader(http_code = 304, "Not Modified");//leaving off third arg leaves off ContentLength header

    endHeader();

    reply = nullptr; //free(), don't just forget and hope we remember to free it later: reply_length = 0;
    reply_type = REPLY_GENERATED;
    header_only = true;
    return;
  }

  if (range_begin_given) { //was pointless to check range_end_given after checking begin, we never set the latter unless we have set the former
    off_t from = ~0; //init to ridiculous value ...
    off_t to = ~0; //... so that things will blow up if we don't actually set the fields

    if (range_end_given) {
      /* 100-200 */
      from = range_begin;
      to = range_end;

      /* clamp end to filestat.st_size-1 */
      if (to > (filestat.st_size - 1)) {
        to = filestat.st_size - 1;
      }
    } else if (range_begin_given && !range_end_given) {
      /* 100- :: yields 100 to end */
      from = range_begin;
      to = filestat.st_size - 1;
    } else if (!range_begin_given && range_end_given) {
      /* -200 :: yields last 200 */
      to = filestat.st_size - 1;
      from = to - range_end + 1;

      /* clamp start */
      if (from < 0) {
        from = 0;
      }
    } else {
      errx(1, "internal error - from/to mismatch");
    }

    if (from >= filestat.st_size) {
      default_reply(416, "Requested Range Not Satisfiable", "You requested a range outside of the file.");
      return;
    }

    if (to < from) {
      default_reply(416, "Requested Range Not Satisfiable", "You requested a backward range.");
      return;
    }
    reply_start = from;
    file_length = to - from + 1;

    startCommonHeader( 206,"Partial Content",file_length);
    header_fd.printf(   "Content-Range: bytes %llu-%llu/%llu\r\n",llu(from), llu(to),llu(filestat.st_size));

    header_fd.printf(   "Content-Type: %s\r\n",mimetype.pointer);
    header_fd.printf(      "Last-Modified: %s\r\n" , lastmod);

    endHeader();

    debug("sending %llu-%llu/%llu\n", llu(from), llu(to), llu(filestat.st_size));
  } else {
    /* no range stuff */
    file_length = filestat.st_size;
startCommonHeader(200,"OK",file_length);

    header_fd.printf(   "Content-Type: %s\r\n",mimetype.pointer);
    header_fd.printf(      "Last-Modified: %s\r\n" , lastmod);
    endHeader();
  }
}

/* Returns 1 if passwords are equal, runtime is proportional to the length of
 * user_input to avoid leaking the secret's length and contents through timing
 * information.
 */
bool password_equal(const char *user_input, const char *secret) {
  if (user_input == nullptr) {
    return false; //
  }
  size_t i = 0;
  size_t j = 0;
  char out = 0;

  while (true) {
    /* Out stays zero if the strings are the same. */
    out |= user_input[i] ^ secret[j];

    /* Stop at end of user_input. */
    if (user_input[i] == 0) {
      break;
    }
    i++;

    /* Don't go past end of secret. */
    if (secret[j] != 0) {
      j++;
    }
  }

  /* Check length after loop, otherwise early exit would leak length. */
  out |= (i != j); /* Secret was shorter. */
  out |= (secret[j] != 0); /* Secret was longer; j is not the end. */
  return out == 0;
}

/* Process a request: build the header and reply, advance state. */
void DarkHttpd::Connection::process_request() {
  service.fyi.num_requests++;

  if (!parse_request()) {
    default_reply(400, "Bad Request", "You sent a request that the server couldn't understand.");
  } else if (is_https_redirect()) {
    redirect_https();
  }
  /* fail if: (auth_enabled) AND (client supplied invalid credentials) */
  else if (service.auth_key != nullptr && !password_equal(authorization, service.auth_key)) { // todo:extract method
    default_reply(401, "Unauthorized", "Access denied due to invalid credentials.");
  } else if (strcmp(method, "GET") == 0) {
    process_get();
  } else if (strcmp(method, "HEAD") == 0) {
    process_get();
    header_only = true;
  } else {
    default_reply(501, "Not Implemented", "The method you specified is not implemented.");
  }

  /* advance state */
  state = SEND_HEADER;

  /* request not needed anymore */
  request = nullptr;
}

/* Receiving request. */
void DarkHttpd::Connection::poll_recv_request() {
  char buf[1 << 15];

  assert(state == RECV_REQUEST);
  ssize_t recvd = recv(socket, buf, sizeof(buf), 0);
  debug("poll_recv_request(%d) got %d bytes\n", int(socket), (int) recvd);
  if (recvd < 1) {
    if (recvd == -1) {
      if (errno == EAGAIN) {
        debug("poll_recv_request would have blocked\n");
        return;
      }
      debug("recv(%d) error: %s\n", int(socket), strerror(errno));
    }
    conn_closed = true;
    state = DONE;
    return;
  }
  last_active = service.now;

  /* append to conn->request */
  assert(recvd > 0);
  bool ok = request.cat(buf, recvd);
  if (!ok) {
    debug("failed to append chunk to request being received");
    //need to except.
  }
  auto newLength = request.length + recvd + 1;
  request = static_cast<char *>(xrealloc(request, newLength));
  memcpy(&request.pointer[request.length], buf, recvd);
  request.length += recvd;
  request.pointer[request.length] = 0;
  service.fyi.total_in += recvd;

  /* process request if we have all of it */
  if (request.endsWith("\n\n", 2) || request.endsWith("\r\n\r\n", 4)) {
    process_request();
  }

  /* die if it's too large */
  if (request.length > MAX_REQUEST_LENGTH) {
    default_reply(413, "Request Entity Too Large", "Your request was dropped because it was too long.");
    state = SEND_HEADER;
  }

  /* if we've moved on to the next state, try to send right away, instead of
   * going through another iteration of the select() loop.
   */
  if (state == SEND_HEADER) {
    poll_send_header();
  }
}

/* Sending header.  Assumes conn->header is not NULL. */
void DarkHttpd::Connection::poll_send_header() {
  ssize_t sent;

  assert(state == SEND_HEADER);
  assert(header.length == strlen(header));

  sent = send(socket, &header[header_sent], header.length - header_sent, 0);
  last_active = service.now;
  debug("poll_send_header(%d) sent %d bytes\n", int(socket), (int) sent);

  /* handle any errors (-1) or closure (0) in send() */
  if (sent < 1) {
    if (sent == -1 && errno == EAGAIN) {
      debug("poll_send_header would have blocked\n");
      return;
    }
    if (sent == -1) {
      debug("send(%d) error: %s\n", int(socket), strerror(errno));
    }
    conn_closed = true;
    state = DONE;
    return;
  }
  assert(sent > 0);
  header_sent += sent;
  total_sent += sent;
  service.fyi.total_out += sent;

  /* check if we're done sending header */
  if (header_sent == header.length) {
    if (header_only) {
      state = DONE;
    } else {
      state = SEND_REPLY;
      /* go straight on to body, don't go through another iteration of
       * the select() loop.
       */
      poll_send_reply();
    }
  }
}

/* Send chunk on socket <s> from FILE *fp, starting at <ofs> and of size
 * <size>.  Use sendfile() if possible since it's zero-copy on some platforms.
 * Returns the number of bytes sent, 0 on closure, -1 if send() failed, -2 if
 * read error.
 *
 * TODO: send headers with sendfile(), this will result in fewer packets.
 */
static ssize_t send_from_file(const int s, const int fd, off_t ofs, size_t size) {
#ifdef __FreeBSD__
  off_t sent;
  int ret = sendfile(fd, s, ofs, size, NULL, &sent, 0);

  /* It is possible for sendfile to send zero bytes due to a blocking
   * condition.  Handle this correctly.
   */
  if (ret == -1) {
    if (errno == EAGAIN) {
      if (sent == 0) {
        return -1;
      } else {
        return sent;
      }
    } else {
      return -1;
    }
  } else {
    return size;
  }
#else
#if defined(__linux) || defined(__sun__)
  /* Limit truly ridiculous (LARGEFILE) requests. */
  if (size > 1 << 20) { //this is only 1 megabyte, with such a limit so much more of this code is pointless.
    size = 1 << 20;
  }
  return sendfile(s, fd, &ofs, size);
#else
  /* Fake sendfile() with read(). */
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
  char buf[1 << 15];
  size_t amount = min(sizeof(buf), size);
  ssize_t numread;

  if (lseek(fd, ofs, SEEK_SET) == -1) {
    err(1, "fseek(%d)", (int) ofs);
  }
  numread = read(fd, buf, amount);
  if (numread == 0) {
    fprintf(stderr, "premature eof on fd %d\n", fd);
    return -1;
  } else if (numread == -1) {
    fprintf(stderr, "error reading on fd %d: %s", fd, strerror(errno));
    return -1;
  } else if ((size_t) numread != amount) {
    fprintf(stderr, "read %zd bytes, expecting %zu bytes on fd %d\n",
      numread, amount, fd);
    return -1;
  } else {
    return send(s, buf, amount, 0);
  }
#endif
#endif
}

/* Sending reply. */
void DarkHttpd::Connection::poll_send_reply() {
  ssize_t sent;

  assert(state == SEND_REPLY);
  assert(!header_only);
  if (reply_type == REPLY_GENERATED) {
    assert(reply.length >= reply_sent);
    sent = send(socket, &reply[reply_start + reply_sent], reply.length - reply_sent, 0);
  } else {
    /* off_t of file_length can be wider than size_t, avoid overflow in send_len */
    const size_t max_size_t = ~0;
    off_t send_len = file_length - reply_sent;
    if (send_len > max_size_t) {
      send_len = max_size_t;
    }
    errno = 0;
    assert(reply.length >= reply_sent);
    sent = send_from_file(socket, reply_fd, reply_start + reply_sent, send_len);
    if (sent < 1) {
      debug("send_from_file returned %lld (errno=%d %s)\n", llu(sent), errno, strerror(errno));
    }
  }
  last_active = service.now;
  debug("poll_send_reply(%d) sent %ld: %llu+[%llu-%llu] of %llu\n", int(socket), sent, llu(reply_start), llu(reply_sent), llu(reply_sent + sent - 1), llu(file_length));

  /* handle any errors (-1) or closure (0) in send() */
  if (sent < 1) {
    if (sent == -1) {
      if (errno == EAGAIN) {
        debug("poll_send_reply would have blocked\n");
        return;
      }
      debug("send(%d) error: %s\n", int(socket), strerror(errno));
    } else if (sent == 0) {
      debug("send(%d) closure\n", int(socket));
    }
    conn_closed = true;
    state = DONE;
    return;
  }
  reply_sent += sent;
  total_sent += sent;
  service.fyi.total_out += sent;

  /* check if we're done sending */
  if (reply_sent == file_length) {
    state = DONE;
  }
}

/* Main loop of the httpd - a select() and then delegation to accept
 * connections, handle receiving of requests, and sending of replies.
 */
void DarkHttpd::httpd_poll() {
  bool bother_with_timeout = false;

  timeval timeout;
  timeout.tv_sec = timeout_secs;
  timeout.tv_usec = 0;

  fd_set recv_set;
  FD_ZERO(&recv_set);
  fd_set send_set;
  FD_ZERO(&send_set);
  int max_fd = 0;

  /* set recv/send fd_sets */
#define MAX_FD_SET(sock, fdset)               \
  do {                                        \
    FD_SET(sock, fdset);                      \
    max_fd = (max_fd < sock) ? sock : max_fd; \
  } while (0)

  if (accepting) {
    MAX_FD_SET(int(sockin), &recv_set);
  }

  for (auto conn: entries) {
    switch (conn->state) {
      case Connection::DONE:
        /* do nothing, no connection should be left in this state */
        break;

      case Connection::RECV_REQUEST:
        MAX_FD_SET(int(conn->socket), &recv_set);
        bother_with_timeout = true;
        break;

      case Connection::SEND_HEADER:
      case Connection::SEND_REPLY:
        MAX_FD_SET(int(conn->socket), &send_set);
        bother_with_timeout = true;
        break;
    }
  }
#undef MAX_FD_SET

#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
  __msan_unpoison(&recv_set, sizeof(recv_set));
  __msan_unpoison(&send_set, sizeof(send_set));
#endif
#endif

  /* -select- */
  if (timeout_secs == 0) {
    bother_with_timeout = false;
  }

  timeval t0;
  if (debug("select() with max_fd %d timeout %d\n", max_fd, bother_with_timeout ? (int) timeout.tv_sec : 0)) {
    gettimeofday(&t0, nullptr);
  }
  int select_ret = select(max_fd + 1, &recv_set, &send_set, nullptr, bother_with_timeout ? &timeout : nullptr);
  if (select_ret == 0) {
    if (!bother_with_timeout) {
      errx(1, "select() timed out");
    }
  }
  if (select_ret == -1) {
    if (errno == EINTR) {
      return; /* interrupted by signal */
    } else {
      err(1, "select() failed");
    }
  }
  if (debug(nullptr)) {
    timeval t1;
    gettimeofday(&t1, nullptr);
    long long sec = t1.tv_sec - t0.tv_sec;
    long long usec = t1.tv_usec - t0.tv_usec;
    if (usec < 0) {
      usec += 1000000;
      sec--;
    }
    printf("select() returned %d after %lld.%06lld secs\n", select_ret, sec, usec);
  }

  /* update time */
  now.refresh(); //read clock and textify

  /* poll connections that select() says need attention */
  if (FD_ISSET(int(sockin), &recv_set)) {
    accept_connection();
  }

  for (auto conn: entries) {
    conn->poll_check_timeout();
    int socket = conn->socket;
    switch (conn->state) {
      case Connection::RECV_REQUEST:
        if (FD_ISSET(socket, &recv_set)) {
          conn->poll_recv_request();
        }
        break;

      case Connection::SEND_HEADER:
        if (FD_ISSET(socket, &send_set)) {
          conn->poll_send_header();
        }
        break;

      case Connection::SEND_REPLY:
        if (FD_ISSET(socket, &send_set)) {
          conn->poll_send_reply();
        }
        break;

      case Connection::DONE:
        /* (handled later; ignore for now as it's a valid state) */
        break;
    }

    /* Handling SEND_REPLY could have set the state to done. */
    if (conn->state == Connection::DONE) {
      /* clean out finished connection */
      if (conn->conn_closed) {
        entries.remove(conn);
        conn->clear();
      } else {
        conn->recycle();
      }
    }
  }
}

/* Daemonize helpers. */
#define PATH_DEVNULL "/dev/null"

bool DarkHttpd::daemonize_start() {
  if (!lifeline.connect()) {
    err(1, "pipe(lifeline)");
    return false;
  }

  fd_null = open(PATH_DEVNULL, O_RDWR, 0);
  if (!fd_null) {
    err(1, "open(" PATH_DEVNULL ")");
  }

  pid_t f = fork();
  if (f == -1) {
    err(1, "fork");
    return false;
  } else if (f != 0) {
    /* parent: wait for child */
    char tmp[1];
    int status;

    if (close(lifeline[1]) == -1) {
      warn("close lifeline in parent");
    }
    if (read(lifeline[0], tmp, sizeof(tmp)) == -1) {
      warn("read lifeline in parent");
    }
    pid_t w = waitpid(f, &status, WNOHANG);
    if (w == -1) {
      err(1, "waitpid");
      return false;
    } else if (w == 0) {
      /* child is running happily */
      exit(EXIT_SUCCESS);
      return true;
    } else {
      /* child init failed, pass on its exit status */
      exit(WEXITSTATUS(status));
      return false;
    }
  }
  /* else we are the child: continue initializing */
  return true;
}

void DarkHttpd::daemonize_finish() {
  if (!fd_null) {
    return; /* didn't daemonize_start() so we're not daemonizing */
  }

  if (setsid() == -1) {
    err(1, "setsid");
  }

  lifeline.close();

  /* close all our std fds */
  if (!fd_null.copyinto(STDIN_FILENO)) {
    warn("dup2(stdin)");
  }
  if (!fd_null.copyinto(STDOUT_FILENO)) {
    warn("dup2(stdout)");
  }
  if (!fd_null.copyinto(STDERR_FILENO)) {
    warn("dup2(stderr)");
  }
  if (fd_null.isNotStd()) {
    close(fd_null);
  }
}


void DarkHttpd::change_root() {
#ifdef HAVE_NON_ROOT_CHROOT
  /* We run this even as root, which should never be a bad thing. */
  int arg = PROC_NO_NEW_PRIVS_ENABLE;
  int error = procctl(P_PID, (int) getpid(), PROC_NO_NEW_PRIVS_CTL, &arg);
  if (error != 0) {
    err(1, "procctl");
  }
#endif

  tzset(); /* read /etc/localtime before we chroot */
  if (want_single_file) {
    AutoString path = wwwroot.clone();
    auto lastSlash = strrchr(path, '/');
    /* wwwroot file is not presumed to be in the current directory */
    if (lastSlash) {
      lastSlash[0] = '\0'; // truncate path, wwwroot still as given
      if (chdir(path) == -1) {
        err(1, "chdir(%s)", path.pointer);
      }
      wwwroot = &lastSlash[1];
    } else { //should be same as cwd
      path[0] = '.'; //?! this trusts that wwwroot has at least one char before its null.
      path[1] = '\0';
    }
    if (chroot(path) == -1) {
      err(1, "chroot(%s)", path.pointer);
    }
    printf("chrooted to `%s'\n", path.pointer);
  } else {
    if (chdir(wwwroot.begin()) == -1) {
      err(1, "chdir(%s)", wwwroot.begin());
    }
    if (chroot(wwwroot.begin()) == -1) {
      err(1, "chroot(%s)", wwwroot.begin());
    }
    printf("chrooted to `%s'\n", wwwroot.begin());
    wwwroot.length = 0; /* empty string */
  }
}

/* Close all sockets and FILEs and exit. */
void DarkHttpd::stop_running(int sig unused) {
  forSignals->running = false;
}

// too soon, giving me gried with deleted functions that are the main reason ostream exists.
// std::ostream operator<<( std::ostream & lhs, const struct timeval & rhs) {
//   return lhs << rhs.tv_sec <<'.'<<rhs.tv_usec / 10000;//todo: fixed with zero filled format for second field
// }

/* usage stats */
void DarkHttpd::reportStats() const {
  rusage r;
  getrusage(RUSAGE_SELF, &r);
  printf("CPU time used: %u.%02u user, %u.%02u system\n",
    static_cast<unsigned int>(r.ru_utime.tv_sec),
    static_cast<unsigned int>(r.ru_utime.tv_usec),
    static_cast<unsigned int>(r.ru_stime.tv_sec),
    static_cast<unsigned int>(r.ru_stime.tv_usec / 10000));
  printf("Requests: %llu\n", llu(fyi.num_requests));
  printf("Bytes: %llu in, %llu out\n", llu(fyi.total_in), llu(fyi.total_out));
}

void DarkHttpd::prepareToRun() {
  xasprintf(keep_alive_field, "Keep-Alive: timeout=%d\r\n", timeout_secs);
  if (want_server_id) {
    xasprintf(server_hdr, "Server: %s\r\n", pkgname);
  } else {
    server_hdr = xstrdup("");
    server_hdr.length = 0;
  }
  init_sockin();

  /* open logfile */
  if (logfile_name == nullptr) {
    logfile = stdout;
  } else {
    logfile = fopen(logfile_name, "ab");
    if (logfile == nullptr) {
      err(1, "opening logfile: fopen(\"%s\")", logfile_name);
    }
  }

  if (want_daemon) {
    daemonize_start();
  }

  /* signals */
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    err(1, "signal(ignore SIGPIPE)");
  }
  // to make the following work we have to only have one DarkHttpd per process. We'd have to make a registry of them and match process to something the signal provides.
  if (signal(SIGINT, stop_running) == SIG_ERR) {
    err(1, "signal(SIGINT)");
  }
  if (signal(SIGTERM, stop_running) == SIG_ERR) {
    err(1, "signal(SIGTERM)");
  }

  /* security */
  if (want_chroot) {
    change_root();
  }
  if (drop_gid != INVALID_GID) {
    gid_t list[1];
    list[0] = drop_gid;
    if (setgroups(1, list) == -1) {
      err(1, "setgroups([%d])", (int) drop_gid);
    }
    if (setgid(drop_gid) == -1) {
      err(1, "setgid(%d)", (int) drop_gid);
    }
    printf("set gid to %d\n", (int) drop_gid);
  }
  if (drop_uid != INVALID_UID) {
    if (setuid(drop_uid) == -1) {
      err(1, "setuid(%d)", (int) drop_uid);
    }
    printf("set uid to %d\n", (int) drop_uid);
  }

    pid.create();


  if (want_daemon) {
    daemonize_finish();
  }
}

void DarkHttpd::freeall() {
  /* close and free connections */

  for (auto conn: entries) {
    entries.remove(conn);
    conn->clear();
  }

  forward_map.clear(); // todo; free contents first! Must establish that all were malloc'd
}

/* Execution starts here. */
int DarkHttpd::main(int argc, char **argv) {
  int exitcode = 0;
  try {
    printf("%s, %s.\n", pkgname, copyright);
    parse_commandline(argc, argv);
    /* NB: parse_commandline() might override parts of the extension map by
     * parsing a user-specified file. THat is why we use insert_or_add when parsing it into the map.
     */
    prepareToRun();
    /* main loop */
    running = true;
    while (running) {
      httpd_poll();
    }
    /* clean exit */
    xclose(sockin);
    if (pid.file_name) {
      pid.remove(); //systemd can be configured to do this for you.
    }
  } catch (DarkException ex) {
    printf("Exit %d attempted, ending polling loop in __FUNCTION__", ex.returncode);
    exitcode = ex.returncode;
  } catch (...) {
    printf("Unknown exception, probably from the std lib");
    exitcode = EXIT_FAILURE;
  }
  if (logfile) {
    fclose(logfile); //guarantees we don't lose a final message.
  }
  freeall(); //gratuitous, ending a process makes this moot.
  reportStats();
  return exitcode;
}

void DarkHttpd::PipePair::close() {
  if (!fds[0].close()) {
    warn("close read end of lifeline in child");
  }
  if (!fds[1].close()) {
    warn("couldn't cut the lifeline");
  }
}

void DarkHttpd::PidFiler::remove() {
  if (unlink(file_name) == -1) {
    err(1, "unlink(pidfile) failed");
  }
  /* if (flock(pidfile_fd, LOCK_UN) == -1)
             err(1, "unlock(pidfile) failed"); */
  close();
}

void DarkHttpd::PidFiler::Remove(const char *why) {
  int error= errno;
  remove();
  errno = error;
  err(1, why);//ignore format-security warning, it can't work here.
}

int DarkHttpd::PidFiler::file_read() {
  char buf[16];
  long long pid;

  Fd::operator=(open(file_name, O_RDONLY));
  if (!seemsOk()) {
    err(1, " after create failed");
  }

  auto i = read(fd, buf, sizeof(buf) - 1);
  if (i == -1) {
    err(1, "read from pidfile failed");
  }
  close();
  buf[i] = '\0';

  if (!str_to_num(buf, &pid)) {
    err(1, "invalid pidfile contents: \"%s\"", buf);
  }
  return pid;
}

#define PIDFILE_MODE 0600

void DarkHttpd::PidFiler::create() {
  if (!file_name) {
    return;
  }

  char pidstr[16];

  /* Open the PID file and obtain exclusive lock. */
  fd = open(file_name,    O_WRONLY | O_CREAT | O_EXLOCK | O_TRUNC | O_NONBLOCK, PIDFILE_MODE);
  if (fd == -1) {
    if ((errno == EWOULDBLOCK) || (errno == EEXIST)) {
      errx(1, "daemon already running with PID %d", file_read());
    } else {
      err(1, "can't create pidfile %s", file_name);
    }
  }

  if (ftruncate(fd, 0) == -1) {
    Remove("ftruncate() failed");
  }

  snprintf(pidstr, sizeof(pidstr), "%d", getpid());
  if (pwrite(fd, pidstr, strlen(pidstr), 0) != strlen(pidstr)) {
    Remove("pwrite() failed");
  }
}

#if 0
these three actions were once associated with closing a connection, but apply only to the server. That seems like a bug, limiting the server to single connections despite all the work to have multiple ones.
log_connection(this);
xclose(reply_fd);
/* If we ran out of sockets, try to resume accepting. */
accepting = true;
#endif
