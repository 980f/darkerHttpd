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
 * todo: epoll instead of select. Grr,no epoll on FreeBSD or NetBSD so we will use ppoll instead.
*/

#include "darkhttpd.h"

#include <addr6.h>
#include <cheaptricks.h>
#include <fd.h>
#include <functional>
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
static ssize_t send_from_file(int s, int fd, off_t ofs, size_t size);

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
// #include <netinet/in.h>
#include <netinet/tcp.h>
#include <pwd.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <wait.h>
using namespace DarkHttpd;

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


//for printf, make it easy to know which token to use by forcing the data to the largest integer supported by it.
static_assert(sizeof(unsigned long long) >= sizeof(off_t), "inadequate ull, not large enough for an off_t");

template<typename Integrish> auto llu(Integrish x) {
  return static_cast<unsigned long long>(x);
}

// replaced err.h usage  with logging and throwing an exception.

/* err - prints "error: format: strerror(errno)" to stderr and exit()s with
 * the given code.
 */
static bool err(int code, const char *format, ...) checkFargs(2, 3);

static bool err(int code, const char *format, ...) {
  fprintf(stderr, "err[%d]: %s", code, code > 0 ? strerror(code) : "is not an errno.");
  va_list va;
  va_start(va, format);

  vfprintf(stderr, format, va);
  va_end(va);
  throw DarkException(code);
  return false;
}

static void warn(const char *format, ...) checkFargs(1, 2);

static void warn(const char *format, ...) {
  va_list va;
  va_start(va, format);
  vfprintf(stderr, format, va);
  fprintf(stderr, ": %s\n", strerror(errno));
  va_end(va);
}

/* close() that dies on error.  */
static void xclose(Fd fd) {
  if (!fd.close()) {
    err(1, "close()");
  }
}

// removed  strntoupper as we are quite careful to enforce nulls on the end of anything we upperify.
/* malloc that dies if it can't allocate. */
static void *xmalloc(const size_t size) {
  void *ptr = malloc(size);
  if (ptr == nullptr) {
    err(-1, "can't allocate %zu bytes", size);
  }
  return ptr;
}

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

/* Resolve /./ and /../ in a URL, in-place.
 * @returns whether @param URL is legit.
 * presently bizarre sequences of slashes and dots are treated as if all the slashes preceded all of the dots. That needs to be fixed to handle ../../ and the like.
 */
static bool make_safe_url(StringView url) {
  /* URLs not starting with a slash are illegal. */
  if (*url != '/') {
    return false;
  }

  char *reader = url.begin();
  char *writer = reader;
  // #define ends(c) ((c) == '/' || (c) == '\0')
  unsigned priorSlash = 1; //the leading one
  unsigned priorDot = 0;

  /* Copy to dst, while collapsing multi-slashes and handling dot-dirs. */
  // special cases /./  /../  /a../ /a./  /.../
  while (char c = *++reader) {
    if (c == '/') {
      if (priorDot) {}
      ++priorSlash;
      continue; //action defered until we have seen something other than slash or dot
    }
    if (c == '.') {
      ++priorDot;
      continue;
    }
    //not a slash nor a dot so resolve those
    switch (priorDot) {
      case 1: //trailing dot
        if (take(priorSlash)) {
          *writer++ = '/'; //one slash, and dot disappears
        }
        priorDot = 0;
        continue; //do tdisappears
      case 2:
        //todo: backup to slash prior to counted ones, and emit nothing
        if (take(priorSlash)) {
          while (writer > url) {
            if (*writer == '/') {
              priorDot = 0;
              break;
            }
          }
          if (priorDot) { //we exited the while by running off of the end
            *url = 0; //kill it, kill it really hard
            return false;
          }
        }
      //tried to escape
      default:
        if (priorDot) {
          *url = 0; //kill it, kill it really hard
          return false;
        }
        break;
    }
    if (priorSlash) {
      *writer++ = '/'; //one slash, and dot disappears
      priorSlash = 0;
      //but also emit the char
    }
    *writer++ = c;
  }
  url.truncateAt(writer);
  return true;
}

/////////////////////////////////////////
//will go away with proper signal hooking:
Server *Server::forSignals = nullptr; // trusting BSS zero to clear this.

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
void Server::load_mime_map_file(const char *filename) {
  auto fd = open(filename, 0);
  if (fd > 0) {
    struct stat filestat;
    if (fstat(fd, &filestat) == 0) {
      if (mimeFileContent.malloc(filestat.st_size)) { //#freed in main()
        if (read(fd, mimeFileContent, filestat.st_size) == filestat.st_size) {
          //the malloc member put a null in so we should be ready to roll.
        } else {
          warn("File I/O issue loading mimetypes file, %s, content ignored", filename);
          mimeFileContent.Free();
        }
      }
    }
  }
}


const char *Server::url_content_type(const char *url) {
  if (url) {
    if (auto period = strrchr(url, '.')) {
      StringView extension = const_cast<char *>(++period);
      StringView pool = mimeFileContent ? mimeFileContent : StringView(const_cast<char *>(default_extension_map)); //the const_cast seems dangerous, but we will get a segv if we screw up, we will not get a silent bug.
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
  return default_mimetype ? default_mimetype : "application/octet-stream";
}

const char *Server::get_address_text(const void *addr) const {
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
void Server::init_sockin() {
  sockaddr_in addrin;
#ifdef HAVE_INET6
  SockAddr6 sock6;
#endif
  socklen_t addrin_len;
  int sockopt;

#ifdef HAVE_INET6
  if (inet6) {
    if (!sock6.presentationToNetwork(bindaddr)) {
      err(-1, "malformed --addr argument");
    }
    sockin = socket(PF_INET6, SOCK_STREAM, 0);
  } else
#endif
  {
    memset(&addrin, 0, sizeof(addrin));
    addrin.sin_addr.s_addr = bindaddr ? inet_addr(bindaddr) : INADDR_ANY;
    if (addrin.sin_addr.s_addr == (in_addr_t) INADDR_NONE) {
      err(-1, "malformed --addr argument");
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

#if DarklySupportAcceptanceFilter
  /* enable acceptfilter (this is only available on FreeBSD) */
  if (want_accf) {

    struct accept_filter_arg filt = {"httpready", ""};
    if (setsockopt(sockin, SOL_SOCKET, SO_ACCEPTFILTER, &filt, sizeof(filt)) == -1) {
      fprintf(stderr, "cannot enable acceptfilter: %s\n", strerror(errno));
    } else {
      printf("enabled acceptfilter\n");
    }
  }

#endif
}

void Server::usage(const char *argv0) {
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
#if DarklySupportDaemon
  printf("\t--daemon (default: don't daemonize)\n"
    "\t\tDetach from the controlling terminal and run in the background.\n\n");
  printf("\t--pidfile filename (default: no pidfile)\n"
    "\t\tWrite PID to the specified file. Note that if you are\n"
    "\t\tusing --chroot, then the pidfile must be relative to,\n"
    "\t\tand inside the wwwroot.\n\n");
#endif
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
    "\t\tFiles with unknown extensions are served as this mimetype.\n\n", default_mimetype);
  printf("\t--uid uid/uname, --gid gid/gname (default: don't privdrop)\n"
    "\t\tDrops privileges to given uid:gid after initialization.\n\n");
  printf("\t--chroot (default: don't chroot)\n"
    "\t\tLocks server into wwwroot directory for added security.\n\n");
#ifdef DarklySupportAcceptanceFilter
  printf("\t--accf (default: don't use acceptfilter)\n"
         "\t\tUse acceptfilter. Needs the accf_http kernel module loaded.\n\n");
#endif
  printf("\t--no-keepalive\n"
    "\t\tDisables HTTP Keep-Alive functionality.\n\n");
#if DarklySupportForwarding
  printf("\t--forward host url (default: don't forward)\n"
    "\t\tWeb forward (301 redirect).\n"
    "\t\tRequests to the host are redirected to the corresponding url.\n"
    "\t\tThe option may be specified multiple times, in which case\n"
    "\t\tthe host is matched in order of appearance.\n\n");
  printf("\t--forward-all url (default: don't forward)\n"
    "\t\tWeb forward (301 redirect).\n"
    "\t\tAll requests are redirected to the corresponding url.\n\n");
  printf("\t--forward-https\n"
    "\t\tIf the client requested HTTP, forward to HTTPS.\n" //there is no way to determine the protocol the user requested other than knowing that the typical inetd is only going to send one of the other to this guy and the person launching the program knows which.
    "\t\tThis is useful if darkhttpd is behind a reverse proxy\n" //this should only be set if you know that all incoming traffic is http. The x-forward-proto request header is how a client can bypass the inetd with this support.
    "\t\tthat supports SSL.\n\n");
#endif
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
  printf("\t--header \"Name: Value\"\n"
    "\t\tAdd a custom header to all responses.\n"
    "\t\tNote that each must be quoted and have a colon and a single space after that.\n"
    "\t\tThis option can be specified multiple times, in which case\n"
    "\t\tthe headers are added in order of appearance.\n\n");
#ifndef HAVE_INET6
  printf("\t(This binary was built without IPv6 support: -DNO_IPV6)\n\n");
#endif
}

/* @returns whether string is strictly a number.  Set num to NULL if disinterested in its value.
 */
// static long long str_to_num(const char *str, bool *fault) {
//   char *endptr;
//   long long n;
//
//   errno = 0;
//   n = strtoll(str, &endptr, 10);
//   if (fault) {
//     *fault = (*endptr!=0) || (n == LLONG_MIN && errno == ERANGE) ||(n == LLONG_MAX && errno == ERANGE);
//   }
//   return n;
// }
//
// /* @returns a valid number or dies.
//  * @deprecated
//  */
// static long long xstr_to_num(const char *str) {
//   long long ret=~0;//something likely to blow hard if the parse fails.
//
//   if (!str_to_num(str, &ret)) {
//     err(-1, "number \"%s\" is invalid", str);
//   }
//   return ret;
// }

class CliScanner {
public:
  CliScanner(int argc, char **argv) : argc{argc}, argv{argv} {}

  bool stillHas(unsigned needed) {
    return argi + needed < argc;
  }

  char *operator()() {
    return argv[argi++];
  }

  bool errorReport() {
    return err(-1, "Not enough arguments for %s",argv[argc-1]);
  }


  template<typename StringAssignable>  bool operator>>(StringAssignable &target) {

    if (argi < argc) {
      if constexpr (std::is_integral<StringAssignable>::value) {
    target = std::stoll(argv[argi++]);
} else {
  target = argv[argi++];
}
      return true;
    } else {
      return errorReport();
    }
  }

private:
  int argc;
  char **argv;
  int argi = 0;
};

bool Server::parse_commandline(int argc, char *argv[]) {
  // if (argc < 2 || (argc == 2 && strcmp(argv[1], ) == 0)) {}
  bindport = getuid() ? 8080 : 80; // default depends on whether run as root.
  // custom_hdrs.clear();

  CliScanner arg(argc, argv);
   invocationName=arg();//there is always an arg0

  try {

    if (arg.stillHas(1)) {
      wwwroot = arg();
      if (wwwroot == "--help") {
        usage(argv[0]); /* no wwwroot given */
        // exit(EXIT_SUCCESS);
        return false;
      }
      wwwroot.trimTrailing("/"); //former code only trimmed a single trailing slash, 980f trims multiple.
      if (wwwroot.length == 0) {
        return err(-1, "/path/to/wwwroot cannot be empty, nor / ");
      }
    } else { /* no wwwroot given */
      usage(argv[0]);
    }

    /* walk through the remainder of the arguments (if any) */
    while (arg.stillHas(1)) {
      StringView token = arg();
      if (token == "--port") {
        arg >> bindport;
      } else if (token ==  "--addr")  {
        arg >> bindaddr ;
      } else if (token ==  "--maxconn")  {
        arg>> max_connections;
      } else if (token ==  "--log")  {
        arg>>        logfile_name ;
      } else if (token ==  "--chroot")  {
        want_chroot = true;
#if DarklySupportDaemon
      } else if (token ==  "--daemon")  {
        want_daemon = true;
#endif
      } else if (token ==  "--index")  {
        arg>>        index_name ;
      } else if (token ==  "--no-listing")  {
        no_listing = true;
      } else if (token ==  "--mimetypes")  {
        arg>>        mimeFileName ;
      } else if (token ==  "--default-mimetype")  {
        arg>>        default_mimetype ;
      } else if (token ==  "--uid")  { //username or a number.
        arg>>        drop_uid ;
      } else if (token ==  "--gid")  {
        arg>>        drop_gid ;
#if DarklySupportDaemon
      } else if (token ==  "--pidfile")  {
        arg>>        d.pid.file_name ;
#endif
      } else if (token ==  "--no-keepalive")  {
        want_keepalive = false;
#if   DarklySupportAcceptanceFilter
    } else if (token ==  "--accf")  {
      want_accf = true;
#endif
      } else if (token ==  "--syslog")  {
        syslog_enabled = true;
#if DarklySupportForwarding
      } else if (token ==  "--forward")  {
        if (arg.stillHas(2)) {//having a syntax for a pack hostname:url would be simpler here.
          char *host,*url;
          arg>>host;
          arg>>url;
          forward.map.add(host, url);
        } else {
          err(-1, "--forward needs two following args, host then url ");
        }
      } else if (token ==  "--forward-all")  {
        arg>>        forward.all_url ;
      } else if (token ==  "--forward-https")  {
        forward.to_https = true;
#endif
      } else if (token ==  "--no-server-id")  {
        want_server_id = false;
      } else if (token ==  "--timeout")  {
        arg>>        timeout_secs ;
      } else if (token ==  "--auth")  {
        arg>>auth;//rest of parsing and checking is in operator= of Authorization.
      } else if (token ==  "--header")  {
        char *header;
        arg>>header;
        if (strchr(header, '\n') != nullptr || strstr(header, ": ") == nullptr) {//note: this requires quoting to keep shell from seeing these as two args.
          err(-1, "malformed argument after --header");
        }
        custom_hdrs.push_back(header);
      }
#ifdef HAVE_INET6
      else if (token ==  "--ipv6")  {
        inet6 = true;
      }
#endif
      else {
        err(-1, "unknown argument `%s'", token.pointer);
        return false;
      }
    }

    return true;
  } catch (...) {
    //todo: add specific clauses with specific error messages.
    return false;
  }
}


/* Accept a connection from sockin and add it to the connection queue. */
void Server::accept_connection() {
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
  conn = new Connection(*this, fd); // connections have defaults from the DarkHttpd that creates them.
  connections.push_front(conn);

#ifdef HAVE_INET6
  if (inet6) {
    conn->client = addrin6.sin6_addr;
  } else
#endif
  {
    *reinterpret_cast<in_addr_t *>(&conn->client) = addrin.sin_addr.s_addr;
  }

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
void Server::log_connection(const Connection *conn) {
  AutoString safe_method;
  AutoString safe_url;
  AutoString safe_referer;
  AutoString safe_user_agent;
  char dest[CLF_DATE_LEN];

  if (logfile == nullptr) {
    return;
  }
  if (conn->reply.http_code == 0) {
    return; /* invalid - died in request */
  }
  //   if (!conn->method) {
  //     return; /* invalid - didn't parse - maybe too long */
  //   }
  //
  //   // all the _safe macros can go away if we stream with a encoding translator in the stream instead of this malloc and free and explode methodology.
  // #define make_safe(x)                                                    \
  //   do {                                                                  \
  //     if (conn->x) {                                                      \
  //       safe_## x = static_cast<char *>(xmalloc(strlen(conn->x) * 3 + 1)); \
  //       logencode(conn->x, safe_## x);                                     \
  //     } else {                                                            \
  //       safe_## x = NULL;                                                  \
  //     }                                                                   \
  //   } while (0)
  //
  //   make_safe(method);
  //   make_safe(url);
  //   make_safe(referer);
  //   make_safe(user_agent);
  //
  // #undef make_safe
  //
  // #define use_safe(x) safe_## x ? safe_## x.pointer : ""
  //   if (syslog_enabled) {
  //     syslog(LOG_INFO, "%s - - %s \"%s %s HTTP/1.1\" %d %llu \"%s\" \"%s\"\n",
  //       get_address_text(&conn->client),
  //       clf_date(dest, now),
  //       use_safe(method),
  //       use_safe(url),
  //       conn->http_code,
  //       llu(conn->total_sent),
  //       use_safe(referer),
  //       use_safe(user_agent));
  //   } else {
  //     fprintf(logfile, "%s - - %s \"%s %s HTTP/1.1\" %d %llu \"%s\" \"%s\"\n",
  //       get_address_text(&conn->client),
  //       clf_date(dest, now),
  //       use_safe(method),
  //       use_safe(url),
  //       conn->http_code,
  //       llu(conn->total_sent),
  //       use_safe(referer),
  //       use_safe(user_agent));
  //     fflush(logfile);
  //   }
  //
  // #undef use_safe
}

/* Parse a Range: field into range_begin and range_end.  Only handles the
 * first range if a list is given.  Sets range_{begin,end}_given to true if
 * associated part of the range is given.
 * "Range: bytes=500-999"
 * "Range: - 456 last 456
 * "Range: 789 -  from 789 to end
 */

int Connection::ByteRange::parse(StringView rangeline) {
  //todo: allow range operand type default?
  auto prefix = rangeline.cutToken('=', false);
  if (!prefix) {
    return 498;
  }
  if (prefix != "bytes") {
    return 498; //todo: error range format not supported
  }
  recycle(); //COA, including annoying client giving us more than one Range header

  begin = rangeline.cutNumber();
  if (begin < 0) { // e.g. -456
    end = -take(begin);
    end.given = true;
  } else {
    begin.given = true;
    if (rangeline[0] == '-') {
      end = rangeline.cutNumber();
      end.given = end > 0;
    }
    if (begin.given && end.given) {
      if (end < begin) {
        return 497;
      }
    }
  }
  //an additional range is presently not supported, we should return an error response.
  return 499; //todo: proper value
}

void Connection::Replier::recycle() {
  header_only = false;
  http_code = 0;

  header.recycle(false);
  content.recycle(true);

  start = 0;
  file_length = 0;
  total_sent = 0;
}

Connection::Connection(Server &parent, int fd): socket(fd), service(parent), theRequest{}, method{}, hostname{nullptr}, url{nullptr}, urlParams{nullptr}, referer{nullptr}, user_agent{nullptr}, authorization{nullptr}, if_mod_since{nullptr}, is_https_redirect{false} {
  memset(&client, 0, sizeof(client));
  nonblock_socket(socket);
  state = RECV_REQUEST;
  last_active = service.now;
}

void Connection::clear() {
  received.length = 0;
  parsed.length = 0;
  method = NotMine;
  url = nullptr;
  referer = nullptr;
  user_agent = nullptr;
  authorization = nullptr;
  reply.header.clear();
  reply.content.clear();
}

/* Recycle a finished connection for HTTP/1.1 Keep-Alive. */
void Connection::recycle() {
  clear(); //legacy, separate heap usage clear from the rest.
  debug("free_connection(%d)\n", int(socket));
  xclose(socket);
  range.recycle();
  reply.recycle();
  conn_closed = true; //todo: check original code

  state = RECV_REQUEST; /* ready for another */
}

/* If a connection has been idle for more than timeout_secs, it will be
 * marked as DONE and killed off in httpd_poll().
 */
void Connection::poll_check_timeout() {
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
static void urldecode(StringView url) {
  auto reader = url.begin();
  auto writer = url.begin();
  while (char c = *reader++) {
    if (c == '%' && reader[1] && isxdigit(reader[1]) && reader[2] && isxdigit(reader[2])) {
      *writer++ = HEX_TO_DIGIT(*reader++) << 4 | HEX_TO_DIGIT(*reader++);
      continue;
    }
    *writer++ = c; /* straight copy */
  }
  url.truncateAt(writer);
}

void Connection::startHeader(const int errcode, const char *errtext) {
  if (errcode > 0) {
    reply.http_code = errcode;
  }
  if (!errtext) {
    errtext = ""; //don't want a "(null)" comment which is what some printf's do for a null pointer.
  }
  reply.header.fd.printf("HTTP/1.1 %d %s\r\n", reply.http_code, errtext);
}

void Connection::catDate() {
  reply.header.fd.printf("Date: %s\r\n", service.now.image);
}

void Connection::catServer() {
  if (service.want_server_id) {
    reply.header.fd.printf("Server: %s\r\n", pkgname);
  }
}

void Connection::catFixed(const char *fixedText) {
  reply.header.fd.printf(fixedText);
}

void Connection::catKeepAlive() {
  if (conn_closed) {
    reply.header.fd.printf("Connection: close\r\n");
  } else {
    reply.header.fd.printf("Keep-Alive: timeout=%d\r\n", service.timeout_secs);
  }
}

void Connection::catCustomHeaders() {
  for (auto custom_Hdr: service.custom_hdrs) {
    reply.header.fd.printf("%s\r\n", custom_Hdr);
  }
}

void Connection::catContentLength(off_t off) {
  reply.header.fd.printf("Content-Length: %llu\r\n", llu(off));
}

void Connection::startCommonHeader(int errcode, const char *errtext, off_t contentLenght = ~0UL) {
  startHeader(errcode, errtext);
  catDate();
  catServer();
  catFixed("Accept-Ranges: bytes\r\n");
  catKeepAlive();
  catCustomHeaders();
  if (~contentLenght != 0) {
    catContentLength(contentLenght);
  }
}


void Connection::catAuth() {
  if (service.auth.key.notTrivial()) {
    reply.header.fd.printf("WWW-Authenticate: Basic realm=\"User Visible Realm\"\r\n");
  }
}

void Connection::catGeneratedOn(bool toReply) {
  if (service.want_server_id) {
    (toReply ? reply.content.fd : reply.header.fd).printf("Generated by %s on %s\n", pkgname, service.now.image);
  }
}


void Connection::endHeader() {
  reply.header.fd.printf("\r\n");
}

void Connection::startReply(int errcode, const char *errtext) {
  //todo: open a tempfile

  reply.content.fd.printf("<!DOCTYPE html><html><head><title>%d %s</title></head><body>\n" "<h1>%s</h1>\n", errcode, errtext, errtext);
}

void Connection::addFooter() {
  reply.content.fd.putln("<hr>");
  catGeneratedOn(true);
  reply.content.fd.putln("</body></html>");
}

/* A default reply for any (erroneous) occasion. */
void Connection::error_reply(const int errcode, const char *errname, const char *format, ...) {
  startReply(errcode, errname);
  va_list va;
  va_start(va, format);
  reply.content.fd.vprintln(format, va);
  va_end(va);
  addFooter();
  endReply();

  //todo:file_length is not related to the file length!
  startCommonHeader(errcode, errname, reply.file_length);
  catFixed("Content-Type: text/html; charset=UTF-8\r\n"); //todo: use catMime();
  catAuth();
  endHeader();

  reply.header_only = false;
  /* Reset reply_start in case the request set a range. */
  reply.start = 0;
}

void Connection::endReply() {
  reply.content.fd.close();
  reply.getContentLength();
}

void Connection::redirect(const char *proto, const char *hostname, const char *url) {
  startReply(reply.http_code = 301, "Moved Permanently");
  reply.content.fd.printf("Moved to: <a href=\"");
  if (proto) {
    reply.content.fd.printf(proto);
  }
  if (hostname) {
    reply.content.fd.printf(hostname);
  }
  reply.content.fd.printf(url);

  reply.content.fd.printf("\">");
  if (proto) {
    reply.content.fd.printf(proto);
  }
  if (hostname) {
    reply.content.fd.printf(hostname);
  }
  reply.content.fd.printf(url);

  reply.content.fd.printf("</a>\n");
  addFooter();
  endReply();

  startHeader(301, "Moved Permanently");
  reply.content.fd.printf("Date: %s\r\n", service.now.image);
  catServer();

  /* "Accept-Ranges: bytes\r\n" - not relevant here */
  reply.content.fd.printf("Location: %s%s\r\n", hostname, url);
  catKeepAlive();
  catCustomHeaders();
  catContentLength(reply.file_length);
  catFixed("Content-Type: text/html; charset=UTF-8\r\n"); //todo: use catMime();
  //no auth?
  endHeader();
}

void Connection::redirect_https() {
  /* work out path of file being requested */
  urldecode(url);

  /* make sure it's safe */
  if (!make_safe_url(url)) {
    error_reply(400, "Bad Request", "You requested an invalid URL.");
    return;
  }

  if (!hostname) {
    error_reply(400, "Bad Request", "Missing 'Host' header.");
    return;
  }
  redirect("https://", hostname, url);
}


/* Parse an HTTP request like "GET /urlGoes/here HTTP/1.1" to get the method (GET), the
 * url (/), the referer (if given) and the user-agent (if given).
 */
bool Connection::parse_request() {
  /* parse method */
  StringView scanner = theRequest;
  auto methodToken = scanner.cutToken(' ', false);
  if (!methodToken) {
    return false; //garble or too short to process
  }
  if (methodToken == "GET") { //980f is being sloppy and using a case ignoring compare here, while RFC7230 says it must be case matched.
    method = GET;
  } else if (methodToken == "HEAD") {
    method = HEAD;
  } else {
    method = NotMine;
  }

  url = scanner.cutToken(' ', false);
  if (!url) {
    return false;
  }

  /* work out path of file being requested */
  urldecode(url);

  /* strip out query params */
  urlParams = url.find('?');
  if (urlParams != nullptr) {
    *urlParams++ = '\0'; //modifies url!
  }

  auto proto = scanner.cutToken('\n', false);
  proto.trimTrailing(" \t\r\n");
  //todo: check for http.1.

  //time to parse headers, as they arrive.
  do {
    auto headerline = scanner.cutToken('\n', false);
    auto headername = headerline.cutToken(':', false);
    //#per RFC do NOT trim trailing, we want to not get a match if someone is giving us invalid header names.
    if (!headername) {
      //todo:distinguish eof from end of header
      break;
    }
    if (headername == "Referer") {
      referer = headerline;
      continue;
    }
    if (headername == "User-Agent") {
      user_agent = headerline;
      continue;
    }
    if (headername == "Authorization") {
      authorization = headerline;
      continue;
    }
    if (headername == "If-Modified-Since") {
      if_mod_since = headerline;
      continue;
    }
    if (headername == "Host") {
      hostname = headerline;
      continue;
    }
    if (headername == "X-Forwarded-Proto") {
      auto proto = headerline;
      is_https_redirect == !proto || strcasecmp(proto, "https") == 0;
      continue;
    }
    if (headername == "Connection") {
      headerline.trimTrailing(" \t\r\n");
      if (headerline == "close") {
        //todo: conn_closed = true;
      } else if (headerline == "keep-alive") {
        //todo: conn_closed = false;
      }
      continue;
    }
    //future possiblities: Content-Length: for put's or push's
    if (headername == "Range") {
      range.parse(headerline);
    }
  } while (scanner.notTrivial());
  //
  // /* cmdline flag can be used to deny keep-alive */
  // if (!service.want_keepalive) {
  //   conn_closed = true;
  // }

  return true;
}

static bool file_exists(const char *path) {
  struct stat filestat;
  return stat(path, &filestat) == -1 && errno == ENOENT ? false : true;
}

class HtmlDirLister {
  Connection &conn;


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
      default:
        return false;
    }
  }

  //apbuf was used in one place and is very mundane code. This server is not particularly swift as it is so we drop this in favor of trusting libc and the OS to be good enough at this very frequenly used feature of an OS.

  /* Escape < > & ' " into HTML entities. */
  static void append_escaped(Fd dst, const char *src) {
    while (auto c = *src++) {
      switch (c) {
        case '<':
          dst.printf("&lt;");
          break;
        case '>':
          dst.printf("&gt;");
          break;
        case '&':
          dst.printf("&amp;");
          break;
        case '\'':
          dst.printf("&apos;");
          break;
        case '"':
          dst.printf("&quot;");
          break;
        default:
          dst.printf("%c", c); //todo:putc
      }
    }
  }

public:
  /* Encode string to be an RFC3986-compliant URL part.
   * Contributed by nf.
   */
  static void htmlencode(Fd reply_fd, char *src) {
    static const char hex[] = "0123456789ABCDEF";

    while (char c = *src++) {
      if (!is_unreserved(c)) {
        reply_fd.printf("\%%c%c", hex[c >> 4], hex[c & 0xF]);
      } else {
        reply_fd.printf("%c", c);
      }
    }
  }

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

    /* Make sorted list of files in a directory.  Returns number of entries, or -1
     * if error occurs.
     */
    ~DirectoryListing() {
      for (auto dlent: ing) {
        free(dlent);
      }
      ing.clear();
    }

  public:
    bool operator()(const char *path, bool includeHidden) {
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
  };

  HtmlDirLister(Connection &conn) : conn{conn} {}

  //was generate_dir_listing
  void operator()(const char *path, const char *decoded_url) {
    // size_t maxlen = 3; /* There has to be ".." */
    /** The time formatting that we use in directory listings.
     * An example of the default is 2013-09-09 13:01, which should be compatible with xbmc/kodi. */
    static const char *const DIR_LIST_MTIME_FORMAT = "%Y-%m-%d %R";
    static const unsigned DIR_LIST_MTIME_SIZE = 16 + 1; /* How large the buffer will need to be. */
    DirectoryListing list;
    if (!list(path, true)) { /* then an opendir() failed */
      if (errno == EACCES) {
        conn.error_reply(403, "Forbidden", "You don't have permission to access this URL.");
      } else if (errno == ENOENT) {
        conn.error_reply(404, "Not Found", "The URL you requested was not found.");
      } else {
        conn.error_reply(500, "Internal Server Error", "Couldn't list directory: %s", strerror(errno));
      }
      return;
    }

    conn.startReply(200, "OK");

    conn.reply.content.fd.printf("<!DOCTYPE html>\n<html>\n<head>\n<title>");
    append_escaped(conn.reply.content.fd, decoded_url);
    conn.reply.content.fd.printf(
      "</title>\n"
      "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
      "</head>\n<body>\n<h1>");
    append_escaped(conn.reply.content.fd, decoded_url);
    conn.reply.content.fd.printf("</h1>\n<table border=\"0\">\n");

    for (auto entry: list.ing) {
      conn.reply.content.fd.printf("<tr>td><a href=\"");

      htmlencode(conn.reply.content.fd, entry->name);

      if (entry->is_dir) {
        conn.reply.content.fd.printf("/");
      }
      conn.reply.content.fd.printf("\">");
      append_escaped(conn.reply.content.fd, entry->name);
      if (entry->is_dir) {
        conn.reply.content.fd.printf("/");
      }
      conn.reply.content.fd.printf("</a></td><td>");

      char mtimeImage[DIR_LIST_MTIME_SIZE];
      tm tm;
      localtime_r(&entry->mtime.tv_sec, &tm); //local computer time? should be option between that and a tz from header.
      strftime(mtimeImage, sizeof mtimeImage, DIR_LIST_MTIME_FORMAT, &tm);

      conn.reply.content.fd.printf(mtimeImage);
      conn.reply.content.fd.printf("</td><td>");
      if (!entry->is_dir) {
        conn.reply.content.fd.printf("%10llu", llu(entry->size));
      }
      conn.reply.content.fd.printf("</td></tr>\n");
    }
    conn.reply.content.fd.printf("</table>\n");
    conn.addFooter();
    conn.endReply();

    conn.startCommonHeader(200, "OK", conn.reply.file_length);
    conn.catFixed("Content-Type: text/html; charset=UTF-8\r\n");
    conn.endHeader();
  }
};


/* Process a GET/HEAD request. */
void Connection::process_get() {
  char lastmod[DATE_LEN];

  /* make sure it's safe */
  if (!make_safe_url(url)) {
    error_reply(400, "Bad Request", "You requested an invalid URL.");
    return;
  }
#if DarklySupportForwarding
  auto forward_to = service.forward(hostname);
  if (forward_to) {
    redirect(nullptr, forward_to, url.pointer); //todo: use flag for proto field
    return;
  }
#endif
  const char *mimetype(nullptr);
  AutoString target = url;

  if (url.endsWith('/')) {
    /* does it end in a slash? serve up url/index_name */
    target.cat(service.index_name);
    if (!file_exists(target)) {
      if (service.no_listing) {
        /* Return 404 instead of 403 to make --no-listing
         * indistinguishable from the directory not existing.
         * i.e.: Don't leak information.
         */
        error_reply(404, "Not Found", "The URL you requested was not found.");
        return;
      }
      generate_dir_listing(url, url); //todo: modify this to generate a content file, swapping out the file name and proceding in this module to get it sent.
      return;
    }
    mimetype = service.url_content_type(service.index_name);
  } else {
    /* points to a file */
    mimetype = service.url_content_type(url);
  }

  debug("url=\"%s\", target=\"%s\", content-type=\"%s\"\n", url.pointer, target.pointer, mimetype);

  /* open file */
  reply.content.fd = open(target, O_RDONLY | O_NONBLOCK);

  if (!reply.content.fd) { /* open() failed */
    if (errno == EACCES) {
      error_reply(403, "Forbidden", "You don't have permission to access this URL.");
    } else if (errno == ENOENT) {
      error_reply(404, "Not Found", "The URL you requested was not found.");
    } else {
      error_reply(500, "Internal Server Error", "The URL you requested cannot be returned: %s.", strerror(errno));
    }
    return;
  }

  struct stat filestat;
  if (fstat(reply.content.fd, &filestat) == -1) {
    error_reply(500, "Internal Server Error", "fstat() failed: %s.", strerror(errno));
    return;
  }
  reply.file_length = filestat.st_size;

  /* make sure it's a regular file */
  if (S_ISDIR(filestat.st_mode)) {
    redirect(nullptr, "/", url); //todo:  probably should be same args as other redirect.
    return;
  } else if (!S_ISREG(filestat.st_mode)) {
    error_reply(403, "Forbidden", "Not a regular file.");
    return;
  }

  rfc1123_date(lastmod, filestat.st_mtime);

  /* check for If-Modified-Since, may not have to send */
  if (if_mod_since.notTrivial() && strcmp(lastmod, if_mod_since) <= 0) { //original code compared for equal, making this useless. We want file mod time any time after the given
    debug("not modified since %s\n", if_mod_since.pointer);
    reply.header_only = true;
    startCommonHeader(304, "Not Modified"); //leaving off third arg leaves off ContentLength header, apparently not needed with a 304.
    endHeader();
    return;
  }

  if (range.begin.given) { //was pointless to check range_end_given after checking begin, we never set the latter unless we have set the former
    off_t from = ~0; //init to ridiculous value ...
    off_t to = ~0; //... so that things will blow up if we don't actually set the fields

    if (range.end.given) {
      /* 100-200 */
      from = range.begin;
      to = range.end;

      /* clamp end to filestat.st_size-1 */
      if (to > (filestat.st_size - 1)) {
        to = filestat.st_size - 1;
      }
    } else if (range.begin.given && !range.end.given) {
      /* 100- :: yields 100 to end */
      from = range.begin;
      to = filestat.st_size - 1;
    } else if (!range.begin.given && range.end.given) {
      /* -200 :: yields last 200 */
      to = filestat.st_size - 1;
      from = to - range.end + 1;

      /* clamp start */
      if (from < 0) {
        from = 0;
      }
    } else {
      err(-1, "internal error - from/to mismatch");
    }

    if (from >= filestat.st_size) {
      error_reply(416, "Requested Range Not Satisfiable", "You requested a range outside of the file.");
      return;
    }

    if (to < from) {
      error_reply(416, "Requested Range Not Satisfiable", "You requested a backward range.");
      return;
    }
    reply.start = from;
    reply.file_length = to - from + 1;

    startCommonHeader(206, "Partial Content", reply.file_length);
    reply.header.fd.printf("Content-Range: bytes %llu-%llu/%llu\r\n", llu(from), llu(to), llu(filestat.st_size));

    debug("sending %llu-%llu/%llu\n", llu(from), llu(to), llu(filestat.st_size));
  } else {
    /* no range stuff */
    reply.file_length = filestat.st_size;
    startCommonHeader(200, "OK", reply.file_length);
  }
  reply.header.fd.printf("Content-Type: %s\r\n", mimetype);
  reply.header.fd.printf("Last-Modified: %s\r\n", lastmod);
  endHeader();
}

/* Process a request: build the header and reply, advance state. */
void Connection::process_request() {
  service.fyi.num_requests++;

  if (!parse_request()) {
    error_reply(400, "Bad Request", "You sent a request that the server couldn't understand.");
    return;
  }
#if DarklySupportForwarding
  if (service.forward.to_https && is_https_redirect) { //this seems to forward all traffic to https due to clause of "no X-forward-proto", but it replicates original source's logic.
    redirect_https();
  } else
#endif
  /* fail if: (auth_enabled) AND (client supplied invalid credentials) */
    if (!service.auth(authorization)) {
      error_reply(401, "Unauthorized", "Access denied due to invalid credentials.");
    } else if (method == GET) {
      process_get();
    } else if (method == HEAD) {
      reply.header_only = true; //setting early so that process can skip steps such as just sizing a response rather than generating it.
      process_get();
    } else {
      error_reply(501, "Not Implemented", "The method you specified is not implemented.");
    }
  /* advance state */
  state = SEND_HEADER;
}

/* Receiving request. */
void Connection::poll_recv_request() {
  // char buf[1 << 15]; //32k is excessive, refuse any request that is longer than a header+maximum filename + any options allowed with a '?' for processing a file (of which the only ones of interest are directory listing options).
  assert(state == RECV_REQUEST);
  ssize_t recvd = recv(socket, received.begin(), sizeof(theRequest) - received.length, 0);
  debug("poll_recv_request(%d) got %d bytes\n", int(socket), int(recvd));
  if (recvd == -1) {
    if (errno == EAGAIN) {
      debug("poll_recv_request would have blocked\n");
      return;
    }
    debug("recv(%d) error: %s\n", int(socket), strerror(errno));
    conn_closed = true;
    state = DONE;
    return;
  }
  if (recvd == 0) {
    return; //original asserted here, but a 0 return is legal, while rare.
  }
  last_active = service.now;

  // //from where parse ended
  // char *lookahead = received.begin() + parsed.length;
  //
  // bool ok = request.cat(buf, recvd);
  // if (!ok) {
  //   debug("failed to append chunk to request being received");
  //   //need to except.
  // }
  // auto newLength = request.length + recvd + 1;
  // request = static_cast<char *>(xrealloc(request, newLength));
  // memcpy(&request.pointer[request.length], buf, recvd);
  // request.length += recvd;
  // request.pointer[request.length] = 0;
  // service.fyi.total_in += recvd;
  //
  // /* process request if we have all of it */
  // if (request.endsWith("\n\n", 2) || request.endsWith("\r\n\r\n", 4)) {
  //   process_request();
  // }
  //
  // /* die if it's too large */
  // if (request.length > MAX_REQUEST_LENGTH) {
  //   default_reply(413, "Request Entity Too Large", "Your request was dropped because it was too long.");
  //   state = SEND_HEADER;
  // }

  /* if we've moved on to the next state, try to send right away, instead of
   * going through another iteration of the select() loop.
   */
  if (state == SEND_HEADER) {
    poll_send_header();
  }
}

/* Sending header.  Assumes conn->header is not NULL. */
void Connection::poll_send_header() {
  ssize_t sent;

  assert(state == SEND_HEADER);
  // assert(header.length == strlen(header));

  sent = send_from_file(socket, reply.header.fd, 0, reply.header.fd.getLength());
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
  reply.header.sent += sent;
  reply.total_sent += sent;
  service.fyi.total_out += sent;

  /* check if we're done sending header */
  if (reply.header.sent == reply.header.fd.length) {
    if (reply.header_only) {
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
void Connection::poll_send_reply() {
  ssize_t sent;

  assert(state == SEND_REPLY);
  assert(!reply.header_only);

  /* off_t of file_length can be wider than size_t, avoid overflow in send_len */
  const size_t max_size_t = ~0;
  off_t send_len = reply.file_length - reply.content.sent;
  if (send_len > max_size_t) {
    send_len = max_size_t;
  }
  errno = 0;
  assert(reply.file_length >= reply.content.sent);
  sent = send_from_file(socket, reply.content.fd, reply.start + reply.content.sent, send_len);
  if (sent < 1) {
    debug("send_from_file returned %lld (errno=%d %s)\n", llu(sent), errno, strerror(errno));
  }

  last_active = service.now;
  debug("poll_send_reply(%d) sent %ld: %llu+[%llu-%llu] of %llu\n", int(socket), sent, llu(reply.start), llu(reply.content.sent), llu(reply.content.sent + sent - 1), llu(reply.file_length));

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
  reply.content.sent += sent;
  reply.total_sent += sent;
  service.fyi.total_out += sent;

  /* check if we're done sending */
  if (reply.content.sent == reply.file_length) {
    state = DONE;
  }
}

void Connection::generate_dir_listing(const char *path, const char *decoded_url) {
  //preparing to have different listing generators, such as "system("ls") with '?'params fed to ls as its params.
  // DirLister htmlLinkGenerator(*this);
  // htmlLinkGenerator(path, decoded_url);
  HtmlDirLister(*this)(path, decoded_url);
}

/* Main loop of the httpd - a select() and then delegation to accept
 * connections, handle receiving of requests, and sending of replies.
 */
void Server::httpd_poll() {
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

  for (auto conn: connections) {
    switch (conn->state) {
      case Connection::DONE:
        /* do nothing, no connection should be left in this state */
        break;

      //original code only listens to reads when not 'done' or 'sending'. Done should not be a state!
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
      err(-1, "select() timed out");
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

  for (auto conn: connections) {
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
        connections.remove(conn);
        conn->clear();
      } else {
        conn->recycle();
      }
    }
  }
}

#if DarklySupportDaemon
/* Daemonize helpers. */
#define PATH_DEVNULL "/dev/null"

bool Server::Daemon::start() {
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

void Server::Daemon::finish() {
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
#endif

bool Server::DropPrivilege::validate() {
  if (asGgroup) {
    group *g = getgrnam(byName);
    if (!g) {
      g = getgrgid(atoi(byName));
    }
    if (g) {
      byNumber=g->gr_gid;
      return true;
    }
  } else {
    passwd *p = getpwnam(byName);
    if (!p) {
      p = getpwuid(atoi(byName));
    }
    if (p) {
      byNumber=p->pw_uid;
      return true;
    }
  }
  err(-1, "no such %s: `%s'", asGgroup?"gid":"uid", byName);
  return false;
}

void Server::change_root() {
#ifdef HAVE_NON_ROOT_CHROOT
  /* We run this even as root, which should never be a bad thing. */
  int arg = PROC_NO_NEW_PRIVS_ENABLE;
  int error = procctl(P_PID, (int) getpid(), PROC_NO_NEW_PRIVS_CTL, &arg);
  if (error != 0) {
    err(1, "procctl");
  }
#endif

  tzset(); /* read /etc/localtime before we chroot */

  if (chdir(wwwroot.begin()) == -1) {
    err(1, "chdir(%s)", wwwroot.begin());
  }
  if (chroot(wwwroot.begin()) == -1) {
    err(1, "chroot(%s)", wwwroot.begin());
  }
  printf("chrooted to `%s'\n", wwwroot.begin());
  wwwroot.length = 0; /* empty string */
}

/* Close all sockets and FILEs and exit. */
void Server::stop_running(int sig unused) {
  if (forSignals) {
    forSignals->running = false;
  }
}

// too soon, giving me grief with deleted functions that are the main reason ostream exists.
// std::ostream operator<<( std::ostream & lhs, const struct timeval & rhs) {
//   return lhs << rhs.tv_sec <<'.'<<rhs.tv_usec / 10000;//todo: fixed with zero filled format for second field
// }

/* usage stats */
void Server::reportStats() const {
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

bool Server::prepareToRun() {
  if (mimeFileName) {
    load_mime_map_file(mimeFileName);
  }

  init_sockin();

  /* open logfile */
  if (logfile_name == nullptr) {
    logfile = stdout;
  } else {
    logfile = fopen(logfile_name, "ab");
    if (logfile == nullptr) {
      err(1, "opening logfile: fopen(\"%s\")", logfile_name);
      return false;
    }
  }
#if DarklySupportDaemon
  if (want_daemon) {
    d.start();
  }
#endif
  /* signals */
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    err(1, "signal(ignore SIGPIPE)");
    return false;
  }
  // to make the following work we have to only have one DarkHttpd per process. We'd have to make a registry of them and match process to something the signal provides.
  if (signal(SIGINT, stop_running) == SIG_ERR) {
    err(1, "signal(SIGINT)");
    return false;
  }
  if (signal(SIGTERM, stop_running) == SIG_ERR) {
    err(1, "signal(SIGTERM)");
    return false;
  }

  /* security */
  if (want_chroot) {
    change_root();
  }
  try {
    if (drop_gid) {
      drop_gid.validate();
      gid_t list[1] = {drop_gid};
      if (setgroups(1, list) == -1) {//todo: this seems aggressive/intrusive, I'd rather have the gid drop fail if the user is not already in that supposedly limited group.
        err(1, "setgroups([%u])", unsigned(drop_gid));
        return false;
      }
      if (setgid(drop_gid) == -1) {
        err(1, "setgid(%u)", unsigned(drop_gid));
        return false;
      }
      printf("set gid to %u\n", unsigned(drop_gid));
    }
    if (drop_uid) {
      drop_uid.validate();
      if (setuid(drop_uid) == -1) {
        err(1, "setuid(%u)", unsigned(drop_uid));
        return false;
      }
      printf("set uid to %u\n", unsigned(drop_uid));
    }
  } catch (...) {
    err(1, "uid or gid params neither valid name nor valid id value");
    return false;
  }
#if DarklySupportDaemon
  d.pid.create(); //creating even if it isn't actually going to be used as a soliton enforcer for the daemon state.

  if (want_daemon) {
    d.finish();
  }
#endif
  return true;
}

void Server::freeall() {
  /* close and free connections */
  for (auto conn: connections) {
    connections.remove(conn);
    conn->clear();
  }
#if DarklySupportForwarding
  forward.map.clear(); // todo; free contents first! Must establish that all were malloc'd
#endif
}

#if DarklySupportDaemon
void Server::Daemon::PipePair::close() {
  if (!fds[0].close()) {
    warn("close read end of lifeline in child");
  }
  if (!fds[1].close()) {
    warn("couldn't cut the lifeline");
  }
}

void Server::Daemon::PidFiler::remove() {
  if (::unlink(file_name) == -1) {
    err(1, "unlink(pidfile) failed");
  }
  /* if (flock(pidfile_fd, LOCK_UN) == -1)
             err(1, "unlock(pidfile) failed"); */
  close();
}

void Server::Daemon::PidFiler::Remove(const char *why) {
  int error = errno;
  remove();
  errno = error;
  err(1, why); //ignore format-security warning, it can't work here.
}

bool Server::Daemon::PidFiler::file_read() {

  Fd::operator=(open(file_name, O_RDONLY));
  if (!seemsOk()) {
    return err(1, " after create failed");
  }

  auto i = read(fd, lastRead, sizeof(lastRead) - 1);
  if (i == -1) {
    return err(1, "read from pidfile failed");
  }
  close();
  lastRead[i] = '\0';
  return true;
}

#define PIDFILE_MODE 0600

void Server::Daemon::PidFiler::create() {
  if (!file_name) {
    return;
  }
  /* Open the PID file and obtain exclusive lock. */
  fd = open(file_name, O_WRONLY | O_CREAT | O_EXLOCK | O_TRUNC | O_NONBLOCK, PIDFILE_MODE);
  if (fd == -1) {
    if ((errno == EWOULDBLOCK) || (errno == EEXIST)) {
      if (file_read()) {
        err(-1, "daemon already running with PID %s", lastRead);
      }
    } else {
      err(1, "can't create pidfile %s", file_name);
    }
  }
  //removed as we use O_TRUNC and file won't open if it can't be truncated.
  // if (ftruncate(fd, 0) == -1) {
  //   Remove("ftruncate() failed");
  // }
  lastWrote=getpid();
  printf( "%d", lastWrote);
}
#endif

struct Base64Getter {
  StringView encoded;
  unsigned phase = 0;
  char dregs = ~0;
  bool inputExhausted = false;

  unsigned get6() {
    if (inputExhausted) {
      return ~0;
    }
    char b64 = encoded.chop(1);
    inputExhausted = !encoded;
    switch (b64) {
      case '=':
        inputExhausted = true;
        return ~0;
      case '/':
        return 63;
      case '+':
        return 62;
      default:
        if (b64 <= '9') {
          return b64 - '0';
        }
        if (b64 <= 'Z') {
          return b64 - 'A';
        }
        return b64 - 'a';
    }
  }

  Base64Getter(const StringView &encoded) : encoded{encoded} {}

  operator bool() const {
    return !inputExhausted;
  }

  uint8_t operator()() {
    unsigned acc = 0;
    if (phase == 3) {
      phase = 0;
    }
    switch (phase++) {
      default: //appease compiler, won't ever happen.
      case 0: //6|2
        acc = get6() << 2;
        dregs = get6();
        return acc | (dregs >> 4);
      case 1: //4|4
        acc = dregs & 0xF << 4;
        dregs = get6();
        return acc | (dregs >> 4);
      case 2: //2|6
        acc = (dregs & 0x3) << 6;
        dregs = get6();
        return acc | dregs;
    }
  }
};

/* Returns 1 if passwords are equal, runtime is proportional to the length of
 * user_input to avoid leaking the secret's length and contents through timing
 * information.
 */
bool Server::Authorizer::operator()(StringView user_input) {
  if (!key) {
    return true;
  }
  if (!user_input) {
    return false;
  }
  //Strip "Basic"
  user_input.trimLeading(" \t");
  auto shouldBeBasic = user_input.cutToken(' ', false);

  StringView scanner(key);
  Base64Getter base64(user_input);
  // size_t i = 0;
  // size_t j = 0;
  char out = 0;

  while (base64) {
    /* Out stays zero if the strings are the same. */
    out |= base64() ^ scanner.chop(1);
  }
  out |= scanner.notTrivial();
  while (scanner) { //kill time to defeat timing based attack, a stupid thing given that password length is all that could be determined and that would take a shitload of attempts and we should have rate-limiting for other reasons.
    scanner.chop(1);
    base64();
  }
  return out == 0;
}


/* Blocking invocation of server */
int Server::main(int argc, char **argv) {
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
#if DarklySupportDaemon
    if (d.pid.file_name) {
      d.pid.remove(); //systemd can be configured to do this for you.
    }
#endif
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
  reportStats();
  freeall(); //gratuitous, ending a process makes this moot, except for memory leak detector.
  return exitcode;
}
