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
 * todo: epoll instead of select. Grr,no epoll on FreeBSD or NetBSD so we will use ppoll instead. Well, some dox says FreeBSD does have epoll. Apparently if you have kqueue there is a userspace implementation of epoll.
*/

#include "darkhttpd.h"

#include "addr6.h"
#include "base64getter.h"
#include <cheaptricks.h>
#include <fcntl.h>
#include <limits>
#include <logger.h>
#include <sys/epoll.h>

#include "cliscanner.h"
#include "directorylisting.h"
#include "fd.h"
#include "htmldirlister.h"

static const char pkgname[] = "darkhttpd/1.16.from.git/980f";
static const char copyright[] = "copyright (c) 2003-2024 Emil Mikulic"
  ", totally refactored in 2024 by github/980f";

/* Possible build options: -DDEBUG -DNO_IPV6 */

#define _FILE_OFFSET_BITS 64 /* stat() files bigger than 2GB */
#include <sys/sendfile.h>
//the above and below imply that I garbled something from the original source.
// #ifdef __sun__
// #include <sys/sendfile.h>
// #endif


#include <arpa/inet.h>
#include <cassert>
#include <cctype>
#include <cerrno>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <netinet/tcp.h>
#include <sys/resource.h>  //used by reportstats
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <wait.h>  //used by one of the conditionally compiled blocks, do not remove. TODO: find which conditional flag isinvolved.

using namespace DarkHttpd;
/* Send chunk on socket <s> from FILE *fp, starting at <ofs> and of size
 * <size>.  Use sendfile() if possible since it's zero-copy on some platforms.
 * Returns the number of bytes sent, 0 on closure, -1 if send() failed, -2 if
 * read error.
 *
 * TODO: send headers with sendfile(), this will result in fewer packets.
 */
static ssize_t send_from_file(const int s, const int fd, ByteRange &range) {
  /* off_t of file_length can be wider than size_t, avoid overflow in send_len */

  off_t send_len = std::min(range.getLength(), off_t(std::numeric_limits<size_t>::max()));

  errno = 0;

#ifdef __FreeBSD__
  off_t sent;
  int ret = sendfile(fd, s, ofs, send_len, NULL, &sent, 0);

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
  // send_len=std::min(send_len, 1 << 20);// //this is only 1 megabyte, no good reason is given to apply this to sendfile.
  //NB: range.being.number gets altered by this call, don't reproduce that ourselves!
  return sendfile(s, fd, &range.begin.number, send_len); //O_NONBLOCK needs to be set on opening the socket.
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


#include "darkerror.h" //err function.
/* close() that dies on error.  */
static void xclose(Fd fd) {
  if (!fd.close()) {
    err(1, "close()");
  }
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

  char *reader = url.begin();
  if (*reader != '/') {
    return false;
  }

  char *writer = reader;
  //counts of special chars encountered.
  unsigned priorSlash = 1; //the leading one
  unsigned priorDot = 0;

  /* Copy to dst, while collapsing multi-slashes and handling dot-dirs. */
  // special cases /./  /../  /a../ /a./  /.../
  while (char c = *++reader) {
    if (c == '/') {
      if (priorDot) {}
      ++priorSlash;
      continue; //action deferred until we have seen something other than slash or dot
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
    //todo: shouldn't this be done before we start listening? Doing it after seems to open up a window in which unacceptible connections can queue.
    struct accept_filter_arg filt = {"httpready", ""};
    if (setsockopt(sockin, SOL_SOCKET, SO_ACCEPTFILTER, &filt, sizeof(filt)) == -1) {
      fprintf(stderr, "cannot enable acceptfilter: %s\n", strerror(errno));
    } else {
      printf("enabled acceptfilter\n");
    }
  }

#endif
  epoller.watch(sockin,EPOLLIN, *reinterpret_cast<EpollHandler *>(this)); //the connection socket only has read events. Probably needs HUP's as well though ...
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
    "\t\tFiles with unknown extensions are served as this mimetype.\n\n", contentType.default_type);

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

/** add error reporting to base commandline scanner */
class MyCliScanner : public CliScanner {
public:
  MyCliScanner(int argc, char **argv) : CliScanner(argc, argv) {}


  template<typename StringAssignable> bool operator>>(StringAssignable &target) {
    if (!(CliScanner(*this) >> target)) {
      switch (errno) {
        case EDOM:
          return err(-1, "Not enough arguments for %s", argv[argc - 1]);
        case ERANGE:
          return err(-1, "Numerical value out of range for %s", argv[argc - 1]);
        default:
          return err(errno, "Mysterious error for %s", argv[argc - 1]);
      }
      return false;
    }
    return true;
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

  MyCliScanner arg(argc, argv);
  invocationName = arg(); //there is always an arg0

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
      } else if (token == "--addr") {
        arg >> bindaddr;
      } else if (token == "--maxconn") {
        arg >> max_connections;
      } else if (token == "--log") {
        arg >> log.file_name;
      } else if (token == "--chroot") {
        want_chroot = true;
#if DarklySupportDaemon
      } else if (token == "--daemon") {
        want_daemon = true;
#endif
      } else if (token == "--index") {
        arg >> index_name;
      } else if (token == "--no-listing") {
        no_listing = true;
      } else if (token == "--mimetypes") {
        arg >> contentType.fileName;
      } else if (token == "--create-mimetypes") {
        arg >> contentType.generate;
      } else if (token == "--default-mimetype") {
        arg >> contentType.default_type;
      } else if (token == "--uid") { //username or a number.
        arg >> drop_uid;
      } else if (token == "--gid") {
        arg >> drop_gid;
#if DarklySupportDaemon
      } else if (token == "--pidfile") {
        arg >> d.pid.file_name;
#endif
      } else if (token == "--no-keepalive") {
        want_keepalive = false;
#if   DarklySupportAcceptanceFilter
    } else if (token ==  "--accf")  {
      want_accf = true;
#endif
      } else if (token == "--syslog") {
        log.syslog_enabled = true;
#if DarklySupportForwarding
      } else if (token == "--forward") {
        if (arg.stillHas(2)) { //having a syntax for a pack hostname:url would be simpler here.
          char *host, *url;
          arg >> host;
          arg >> url;
          forward.map.add(host, url);
        } else {
          err(-1, "--forward needs two following args, host then url ");
        }
      } else if (token == "--forward-all") {
        arg >> forward.all_url;
      } else if (token == "--forward-https") {
        forward.to_https = true;
#endif
      } else if (token == "--no-server-id") {
        want_server_id = false;
      } else if (token == "--timeout") {
        arg >> timeout_secs;
      } else if (token == "--auth") {
        arg >> auth; //rest of parsing and checking is in operator= of Authorization.
      } else if (token == "--header") {
        char *header;
        arg >> header;
        if (strchr(header, '\n') != nullptr || strstr(header, ": ") == nullptr) { //note: this requires quoting to keep shell from seeing these as two args.
          err(-1, "malformed argument after --header");
        }
        custom_hdrs.push_back(header);
      }
#ifdef HAVE_INET6
      else if (token == "--ipv6") {
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
    fd = accept(sockin, reinterpret_cast<sockaddr *>(&addrin6), &sin_size);
  } else
#endif
  {
    sin_size = sizeof(addrin);
    memset(&addrin, 0, sin_size);
    fd = accept(sockin, reinterpret_cast<sockaddr *>(&addrin), &sin_size);
  }

  if (fd == -1) {
    /* Failed to accept, but try to keep serving existing connections. */
    if (errno == EMFILE || errno == ENFILE) {
      accepting = false;
    }
    warn("accept()");
    return;
  }
  //todo: see if there is a closed connection object to use. IE pool them.
  /* Allocate and initialize struct connection. */
  conn = new Connection(*this, fd);
  // conn->clear(); //in case we someday pull from pool instead of malloc.
  connections.push_front(conn);
  epoller.watch(fd,EPOLLIN | EPOLLOUT | EPOLLHUP, *reinterpret_cast<EpollHandler *>(this)); //todo: perhaps other EPOLLxxx are needed to catch all connection events?

#ifdef HAVE_INET6
  if (inet6) {
    conn->client = addrin6.sin6_addr;
  } else
#endif
  {
    *reinterpret_cast<in_addr_t *>(&conn->client) = addrin.sin_addr.s_addr;
  }

  debug("accepted connection from %s:%u (fd %d)\n", inet_ntoa(addrin.sin_addr), ntohs(addrin.sin_port), int(conn->socket)); //CLion wrong thinks the cast on conn->socket is not needed.

  /* The accept is due to reception of the start of the request, so there will be data to read */
  conn->poll_recv_request();
}

/* Add a connection's details to the logfile. */
void Connection::logOn(DarkLogger *log) {
  if (!log) {
    return;
  }
  if (reply.http_code == 0) {
    return; /* invalid - died in request */
  }
  if (rq.method == Request::NotMine) {
    return; /* invalid - didn't parse - maybe too long */
  }
  log->tsv(service.get_address_text(&client), last_active.seconds(), rq.method, rq.url, reply.http_code, rq.referer, rq.user_agent);
}

void Connection::Replier::Block::recycle(bool andForget) {
  fd.close();
  if (andForget) { //suspicious fragment in the original, abandoned an open file descriptor, potentially leaking it.
    fd.forget(); // but it might be still open ?!
  }
}

FILE *Connection::Replier::Block::createTemp() {
  return fd.createTemp("darkerhttpdXXXXXX");
}

off_t Connection::Replier::Block::getLength() {
  if (range.begin.given && range.end.given) {
    return range.end.given - range.begin.given;
  } else {
    return -1;
  }
}

void Connection::Replier::clear() {
  header_only = false;
  http_code = 0;

  header.recycle(true);
  content.recycle(true); //todo:1 might be conditional on actual file vs generated content.
}

void Connection::onEpoll(unsigned epoll_flags) {
  if (epoll_flags & EPOLLIN) {
    if (state == RECV_REQUEST) {
      poll_recv_request();
    } else {
      //todo: debug("unexpected input, busy with output or already DONE);
    }
  }
  if (epoll_flags & EPOLLOUT) {
    if (state == SEND_HEADER) {
      poll_send_header();
    } else if (state == SEND_REPLY) {
      poll_send_reply();
    } else {
      //todo: debug("unexpected output notification, while ....");
    }
  }
}

Connection::~Connection() {
  recycle(); //for memory leak test, which should be moot now that we have gotten rid of all dynamically allocated chunks.
  service.epoller.remove(socket);
}

Connection::Connection(Server &parent, int fd): socket(fd), service(parent), rq(service.timeout_secs), reply{} {
  memset(&client, 0, sizeof(client));
  nonblock_socket(socket);
  last_active = service.since(0);
  state = RECV_REQUEST;
}

bool Connection::Request::Lifetime::timeToDie(time_t beenAlive) {
  if (max == 0) {
    max = cli_timeout; //might still be zero.
  }
  if (requested > max) {
    requested = max;
  }

  //order of checks is important!
  if (requested > beenAlive) {
    //do nothing
  } else if (max > beenAlive) {
    //still do nothing
  } else {
    // debug("poll_check_timeout on socket:%d marking connection closed\n", int(socket));
    dieNow = true;
    return true;
    // state = DONE;
  }
  return false;
}

void Connection::Request::clear() {
  received = StringView(theRequest, sizeof(theRequest) - 1); //prepare to receive
  method = Request::NotMine;
  url = nullptr;
  referer = nullptr;
  user_agent = nullptr;
  authorization = nullptr;
  range.clear();
}

Connection::Request::Request(unsigned &cli_timeout): keepalive(cli_timeout) {
  clear();
}

void Connection::clear() {
  rq.clear();
  reply.clear();
}

/* Recycle a finished connection for HTTP/1.1 Keep-Alive. */
void Connection::recycle() {
  clear(); //legacy, separate heap usage clear from the rest.
  debug("free_connection(%d)\n", int(socket));
  xclose(socket);
  rq.keepalive.dieNow = true; //todo: check original code
  state = RECV_REQUEST; /* ready for another */
}

/* If a connection has been idle for more than timeout_secs, it will be
 * marked as DONE and killed off in httpd_poll().
 */
void Connection::poll_check_timeout() {
  if (rq.keepalive.timeToDie(service.since(last_active))) {
    debug("poll_check_timeout on socket:%d marking connection closed\n", int(socket));
    state = DONE;
  }
}


static char HEX_TO_DIGIT(char hex) {
  if (hex >= 'a') {
    return hex - 'a' + 10;
  }
  if (hex >= 'A') {
    return hex - 'A' + 10;
  }
  return hex - '0';
}


/* Decode URL by converting %XX (where XX are hexadecimal digits) to the
 * character it represents.
 */
static void urldecode(StringView &url) {
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
  reply.header.createTemp();

  if (errcode > 0) {
    reply.http_code = errcode;
  }
  if (!errtext) {
    errtext = ""; //don't want a "(null)" comment which is what some printf's do for a null pointer.
  }
  reply.header.fd.printf("HTTP/1.1 %d %s\r\n", reply.http_code, errtext);
}

void Connection::catDate() {
  reply.header.fd.printf("Date: %s\r\n", service.timetText());
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
  if (rq.keepalive.dieNow) {
    reply.header.fd.printf("Connection: close\r\n");
  } else {
    //legacy ignored incoming Keep-alive values and passed server setting back.
    reply.header.fd.printf("Keep-Alive: timeout=%d,max=%d\r\n", rq.keepalive.requested ? rq.keepalive.requested : service.timeout_secs, rq.keepalive.max ? rq.keepalive.max : service.timeout_secs); //Keep-Alive: timeout=5, max=997
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

void Connection::startCommonHeader(int errcode, const char *errtext, off_t contentLength = ~0UL) {
  startHeader(errcode, errtext);
  catDate();
  catServer();
  catFixed("Accept-Ranges: bytes\r\n");
  catKeepAlive();
  catCustomHeaders();
  if (~contentLength != 0) {
    catContentLength(contentLength);
  }
}


void Connection::catAuth() {
  if (service.auth) {
    reply.header.fd.printf("WWW-Authenticate: Basic realm=\"simple file access\"\r\n"); //todo:1 make realm text configurable, a cli in fact since the user might want it to reflect which wwwroot is in use.
  }
}

void Connection::catGeneratedOn(bool toReply) {
  if (service.want_server_id) {
    (toReply ? reply.content.fd : reply.header.fd).printf("Generated by %s on %s\n", pkgname, service.timetText());
  }
}


void Connection::endHeader() {
  reply.header.fd.printf("\r\n");
  reply.header.recordSize();
  //too soon, not until after sending completes. reply.header.fd.close(); //todo: but we need the name so that we can pass that to sendfile.
}

void Connection::startReply(int errcode, const char *errtext) {
  reply.content.createTemp();
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

  startCommonHeader(errcode, errname, reply.content.getLength());
  catFixed("Content-Type: text/html; charset=UTF-8\r\n"); //todo: use catMime();
  catAuth();
  endHeader();

  reply.header_only = false;
}

void Connection::endReply() {
  reply.content.recordSize();
  //too soon, don't close until after sent.  reply.content.fd.close();
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
  reply.content.fd.printf("Date: %s\r\n", service.timetText());
  catServer();

  /* "Accept-Ranges: bytes\r\n" - not relevant here */
  reply.content.fd.printf("Location: %s%s\r\n", hostname, url);
  catKeepAlive();
  catCustomHeaders();
  catContentLength(reply.content.getLength());
  catFixed("Content-Type: text/html; charset=UTF-8\r\n"); //todo: use catMime();
  //no auth?
  endHeader();
}

void Connection::redirect_https() {
  /* work out path of file being requested */
  urldecode(rq.url);

  /* make sure it's safe */
  if (!make_safe_url(rq.url)) {
    error_reply(400, "Bad Request", "You requested an invalid URL.");
    return;
  }

  if (!rq.hostname) {
    error_reply(400, "Bad Request", "Missing 'Host' header.");
    return;
  }
  redirect("https://", rq.hostname, rq.url);
}


/* Parse an HTTP request like "GET /urlGoes/here HTTP/1.1" to get the method (GET), the url (/), and those fields which are of interest to this application, ignoring any that are not.
 * todo: inject nulls so that more str functions can be used.
 */
bool Connection::Request::parse() {
  //restart parse with each chunk until we parse a complete chunk. Seems wasteful but since it is rare that we don't get the whole request header in the first block we are going to keep the code simple.
  StringView scanner(theRequest, received.start); //perhaps -1?

  auto methodToken = scanner.cutToken(' ', false);
  if (!methodToken) {
    return false; //garble or too short to process
  }
  if (methodToken == "GET") { //980f is being sloppy and using a case ignoring compare here, while RFC7230 says it must be case matched.
    method = GET;
  } else if (methodToken == "HEAD") {
    method = HEAD;
  } else {
    method = NotMine; //but we won't formally reject until we see end of header, where we can send a well formed reply
  }

  url = scanner.cutToken(' ', false);
  if (!url) {
    return false;
  }

  /* strip out query params, check for '?' before %xx so that we can accept globs and return a cat of the globbed files in a gzipped chunk. */
  urlParams = url.find('?');
  if (urlParams != nullptr) {
    *urlParams++ = '\0';
  }

  /* canonicalize path, especially guarding against attempts to escape the local root. */
  urldecode(url);

  auto protocol = scanner.cutToken('\n', false);
  protocol.trimTrailing(" \t\r\n"); // \n  is superfluous, but I am hoping that we always use the same 'whitespace' string and can share that.
  //todo: check for http.1.

  do {
    auto headerline = scanner.cutToken('\n', false);
    auto headername = headerline.cutToken(':', false);
    //#per RFC do NOT trim trailing, we want to not get a match if someone is giving us invalid header names.
    if (!headername) {
      //todo:distinguish eof from end of header
      break;
    }
    if (headername == "Referer") { //only used in connection log message :(
      referer = headerline;
      continue;
    }
    if (headername == "User-Agent") { //only used in connection log message :(
      user_agent = headerline;
      continue;
    }
    if (headername == "Authorization") { //RFC7617 requires this be a case insensitive compare.
      authorization = headerline;
      continue;
    }
    if (headername == "If-Modified-Since") {
      if_mod_since = headerline;
      continue;
    }
    if (headername == "Host") { //seems to only be used by forwarding, we may ifdef it away soon.
      hostname = headerline; //Host: <host>[:<port>]
      continue;
    }
    if (headername == "X-Forwarded-Proto") { //seems to also be only for forwarding, should ifdef it away
      is_https_redirect = headerline == "https"; //dropping headerline version of protocol as superfluous. http vs https are not protocol differences for the header body, only for the routing system.
      continue;
    }
    if (headername == "Connection") {
      headerline.trimTrailing(" \t\r\n");
      keepalive.dieNow = headerline == "close";
      //anything else is do linger. //expect another header like "Keep-Alive: timeout=5, max=200"
      continue;
    }
    if (headername == "Keep-Alive") { //a header not to be confused with similar value for Connection:
      while (auto param = headerline.cutToken(',', true)) {
        auto ptoken = param.cutToken('=', false);
        if (!param) {
          //wtf?!  Malformed value for hint parameter
        } else {
          if (ptoken == "timeout") {
            keepalive.requested = atoi(param.begin());
            continue;
          }
          if (ptoken == "max") {
            keepalive.max = atoi(param.begin());
          }
        }
      }
      continue;
    }
    //future possiblities: Content-Length: for put's or push's
    if (headername == "Range") {
      range.parse(headerline);
    }
  } while (scanner.notTrivial());

  return true;
}

ssize_t Connection::Request::receive(int socket) {
  return recv(socket, received.begin(), sizeof(theRequest) - received.length, MSG_DONTWAIT); //MSG_DONTWAIT in case we are wrong about there being at least one byte of data present when a connection is
}

static bool file_exists(const char *path) {
  struct stat filestat;
  return stat(path, &filestat) == -1 && errno == ENOENT ? false : true;
}

/* Process a GET/HEAD request. */
void Connection::process_get() {
  /* make sure it's safe */
  if (!make_safe_url(rq.url)) {
    error_reply(400, "Bad Request", "You requested an invalid URL.");
    return;
  }
#if DarklySupportForwarding
  auto forward_to = service.forward(hostname);
  if (forward_to) {
    redirect(nullptr, forward_to, url.begin()); //todo: use flag for proto field
    return;
  }
#endif
  const char *mimetype(nullptr);
  char target[FILENAME_MAX];
  *rq.url.put(target, true) = 0;

  if (rq.url.endsWith('/')) {
    /* does it end in a slash? serve up url/index_name */
    strcat(target, service.index_name);
    if (!file_exists(target)) {
      urlDoDirectory();
    }
    mimetype = service.contentType(service.index_name);
  } else {
    /* points to a file */
    mimetype = service.contentType(rq.url);
  }

  debug("url=\"%s\", target=\"%s\", content-type=\"%s\"\n", rq.url.begin(), target, mimetype);

  /* open file */
  reply.content.fd = open(target, O_RDONLY | O_NONBLOCK);

  if (!reply.content.fd.seemsOk()) { /* open() failed */
    if (errno == EACCES) {
      error_reply(403, "Forbidden", "You don't have permission to access this URL.");
    } else if (errno == ENOENT) {
      error_reply(404, "Not Found", "The URL you requested was not found.");
    } else {
      error_reply(500, "Internal Server Error", "The URL you requested cannot be returned: %s.", strerror(errno));
    }
    return;
  }

  reply.content.statSize(); //start with full possible size, reduce to requested range later.

  if (!reply.content) {
    error_reply(500, "Internal Server Error", "fstat() failed: %s.", strerror(errno));
    return;
  }

  /* make sure it's a regular file */
  if (reply.content.fd.isDir()) {
    urlDoDirectory();
    return;
  } else if (!reply.content.fd.isRegularFile()) {
    error_reply(403, "Forbidden", "Not a regular file.");
    return;
  }

  Now lastmod(reply.content.fd.getModificationTimestamp(), true); //convert file modification time into rfc1123 standard, rather than convert if_mod_since into time_t

  /* check for If-Modified-Since, may not have to send */
  if (rq.if_mod_since && lastmod <= rq.if_mod_since) { //original code compared for equal, making this useless. We want file mod time any time after the given
    debug("not modified since %s\n", rq.if_mod_since.image);
    reply.header_only = true;
    startCommonHeader(304, "Not Modified"); //leaving off third arg leaves off ContentLength header, apparently not needed with a 304.
    endHeader();
    return;
  }

  if (!!rq.range) {
    //now is the time to shrink the content range to that requested.
    if (!reply.content.range.restrictTo(rq.range)) {
      error_reply(416, "Requested Range Not Satisfiable", "You requested an invalid range or a range outside of the file or the file is not normal.");
    }
    startCommonHeader(206, "Partial Content", reply.content.getLength());
  } else {
    startCommonHeader(200, "OK", reply.content.getLength());
  }
  debug("sending %llu-%llu/%llu\n", llu(reply.content.range.begin), llu(reply.content.range.end), llu(reply.content.fd.getLength()));

  reply.header.fd.printf("Content-Range: bytes %llu-%llu/%llu\r\n", llu(reply.content.range.begin), llu(reply.content.range.end), llu(reply.content.fd.getLength())); //may make this conditional on a partial range.if so just move it into the above 'if'
  reply.header.fd.printf("Content-Type: %s\r\n", mimetype);
  reply.header.fd.printf("Last-Modified: %s\r\n", lastmod);
  endHeader();
}

/* Process a request: build the header and reply, advance state. */
void Connection::process_request() {
  service.fyi.num_requests++;

#if DarklySupportForwarding
  if (service.forward.to_https && is_https_redirect) { //this seems to forward all traffic to https due to clause of "no X-forward-proto", but it replicates original source's logic.
    redirect_https();
  } else
#endif
  /* fail if: (auth_enabled) AND (client supplied invalid credentials) */
  if (!service.auth(rq.authorization)) {
    error_reply(401, "Unauthorized", "Access denied due to invalid credentials.");
  } else if (rq.method == Request::GET) {
    process_get();
  } else if (rq.method == Request::HEAD) {
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
  ssize_t recvd = recv(socket, rq.received.begin(), sizeof(rq.theRequest) - rq.received.length, MSG_DONTWAIT); //MSG_DONTWAIT in case we are wrong about there being at least one byte of data present when a connection is accepted.
  debug("poll_recv_request(%d) got %d bytes\n", int(socket), int(recvd));
  if (recvd == -1) {
    if (errno == EAGAIN) {
      debug("poll_recv_request would have blocked\n");
      return;
    }
    debug("recv(%d) error: %s\n", int(socket), strerror(errno));
    rq.keepalive.dieNow = true;
    state = DONE;
    return;
  }
  if (recvd == 0) {
    return; //original asserted here, but a 0 return is legal, while rare.
  }
  service.fyi.total_in += recvd;
  last_active = service.now();

  *rq.received.begin() = 0; //make the buff into a null terminated string.

  bool readyToRoll = rq.parse();
  /* cmdline flag can be used to deny keep-alive */
  if (!service.want_keepalive) {
    rq.keepalive.dieNow = true; //override parse.
  }


  if (!readyToRoll) {
    //todo: distinguish between not all here yet and too corrupt to process.
    error_reply(400, "Bad Request", "You sent a request that the server couldn't understand.");

    // /* die if it's too large */
    // if (request.length > MAX_REQUEST_LENGTH) {
    //   default_reply(413, "Request Entity Too Large", "Your request was dropped because it was too long.");
    //   state = SEND_HEADER;
    // }
    return;
  }
  /* if we've moved on to the next state, try to send right away, instead of
   * going through another iteration of the select() loop.
   */
  if (state == SEND_HEADER) {
    poll_send_header();
  }
}

int Connection::sendRange(Replier::Block &sending) {
  auto sent = send_from_file(socket, sending.fd, sending.range);
  last_active = service.now(); //keeps alive while shuffling bytes to client.
  debug("sendRange(%d) sent %d bytes\n", int(socket), (int) sent);
  debug("socket(%d) sent %ld: [%llu-%llu] of %s\n", int(socket), sent, llu(sending.range.begin), llu(sending.range.end), "someday the filename will go here");

  /* handle any errors (-1) or closure (0) in send() */
  if (sent < 1) {
    if (sent == -1 && errno == EAGAIN) {
      debug("poll_send_header would have blocked\n");
      return 0; //submitted
    }
    if (sent == -1) {
      debug("send(%d) error: %s\n", int(socket), strerror(errno));
      debug("send_from_file returned %lld (errno=%d %s)\n", llu(sent), errno, strerror(errno));
      return errno;
    }
    //what does zero bytes mean?
    //if header: rq.keepalive.dieNow = true;
    return -1;
  }
  service.fyi.total_out += sent;

  /* check if we're done sending */
  return sending.range.begin >= sending.range.end ? -2 : 0; //>= instead of == while working on off by one issue.
}

/* Sending header.  Assumes conn->header is not NULL. */
void Connection::poll_send_header() {
  switch (sendRange(reply.header)) {
    case -1: //abnormal  termination
      rq.keepalive.dieNow = true;
      state = DONE;
      break;
    case -2: //add data sent
      if (reply.header_only) {
        state = DONE;
      } else {
        state = SEND_REPLY;
        poll_send_reply();
      }
      break;
    default: //some sent ok, expect event on reply.fd
      break;
  }
}


/* Sending reply. */
void Connection::poll_send_reply() {
  switch (sendRange(reply.content)) {
    case -1: //abnormal  termination
      debug("send(%d) closure\n", int(socket));
      rq.keepalive.dieNow = true;
      state = DONE;
      return;
    case -2: //add data sent
      state = DONE;
      return;
    default: //some sent ok, expect event on reply.fd
      break;
  }
}

void Connection::generate_dir_listing(const char *path, const char *decoded_url) {
  //preparing to have different listing generators, such as "system("ls") with '?'params fed to ls as its params.
  HtmlDirLister(*this)(path, decoded_url);
}

void Connection::urlDoDirectory() {
  if (service.no_listing) {
    /* Return 404 instead of 403 to make --no-listing
     * indistinguishable from the directory not existing.
     * i.e.: Don't leak information.
     */
    error_reply(404, "Not Found", "The URL you requested was not found.");
  } else {
    generate_dir_listing(rq.url, rq.url); //todo: modify this to generate a content file, swapping out the file name and proceding in this module to get it sent.
  }
}

/* Main loop of the httpd - a select() and then delegation to accept
 * connections, handle receiving of requests, and sending of replies.
 */
void Server::httpd_poll() {
  // bool bother_with_timeout = false;

  NanoSeconds timeout(timeout_secs);
  //
  // if (accepting) {
  //   epoller.watch(sockin,EPOLLIN,nullptr);
  // }
  //
  // for (auto conn: connections) {
  //   switch (conn->state) {
  //     case Connection::DONE:
  //       /* do nothing, no connection should be left in this state for long */
  //         //might to a prophylactic removal of watch
  //       break;
  //
  //     //original code only listens to reads when not 'done' or 'sending'. Done should not be a state!
  //     case Connection::RECV_REQUEST:
  //       epoller.watch()
  //       MAX_FD_SET(int(conn->socket), &recv_set);
  //       bother_with_timeout = true;
  //       break;
  //
  //     case Connection::SEND_HEADER:
  //     case Connection::SEND_REPLY:
  //       MAX_FD_SET(int(conn->socket), &send_set);
  //       bother_with_timeout = true;
  //       break;
  //     default:
  //       break;
  //   }
  // }


  // if (timeout_secs == 0) {
  //   bother_with_timeout = false;
  // }

  //todo: use safely stopwatch instead of reimplementing it inline
  // timeval t0;
  // if (debug("select() with max_fd %d timeout %d\n", max_fd, bother_with_timeout ? (int) timeout.tv_sec : 0)) {
  //   gettimeofday(&t0, nullptr);
  // }
  if (epoller.loop(timeout)) {
        for (auto conn: connections) {
          conn->poll_check_timeout();
        // int socket = conn->socket;

      //
      /* Handling SEND_REPLY could have set the state to done. */
      if (conn->state == Connection::DONE) {
        /* clean out finished connection */
        if (conn->rq.keepalive.dieNow) {
          //todo: return to pool rather than immediately discarding
          connections.remove(conn);
          delete conn;
        } else { //keeping alive.
          conn->clear();
        }
      }
    }
  } else {
    //todo: debug message about failed poll attempt
  }

  // if (debug(nullptr)) {
  //   timeval t1;
  //   gettimeofday(&t1, nullptr);
  //   long long sec = t1.tv_sec - t0.tv_sec;
  //   long long usec = t1.tv_usec - t0.tv_usec;
  //   if (usec < 0) {
  //     usec += 1000000;
  //     sec--;
  //   }
  //   printf("select() returned %d after %lld.%06lld secs\n", select_ret, sec, usec);
  // }
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

void Server::ReallyDarkLogger::put(Connection::Request::HttpMethods method) {
  switch (method) {
    case Connection::Request::GET:
      DarkLogger::put("GET");
      break;
    case Connection::Request::HEAD:
      DarkLogger::put("HEAD");
      break;
    default:
      DarkLogger::put("Unknown");
      break;
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
    static_cast<unsigned int>(r.ru_utime.tv_usec), //todo: why is this not also divided by 10k like the one below?
    static_cast<unsigned int>(r.ru_stime.tv_sec),
    static_cast<unsigned int>(r.ru_stime.tv_usec / 10000));
  printf("Requests: %llu\n", llu(fyi.num_requests));
  printf("Bytes: %llu in, %llu out\n", llu(fyi.total_in), llu(fyi.total_out));
}

bool Server::prepareToRun() {
  contentType.start();
  init_sockin();

  /* open logfile */
  log.begin();
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
      drop_gid();
    }
    if (drop_uid) {
      drop_uid();
    }
  } catch (...) {
    err(1, "uid or gid params neither valid name nor valid id value or are an attempt to raise rather than drop priority");
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
  lastWrote = getpid();
  printf("%d", lastWrote);
}
#endif

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
  if (shouldBeBasic != "Basic") {
    warn("Auth technique not supported: %s", shouldBeBasic.begin()); //auth will fail to client, this message is to sysadmin.
  }
  StringView scanner(key);
  Base64Getter base64(user_input);
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
    printf("%s, %s.\n", pkgname, copyright); //why is this not using the logging facility?
    running = parse_commandline(argc, argv) && prepareToRun();
    /* main loop */
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
    printf("Exit due to %d, details sent to stderr", ex.returncode); //original code didn't use __F..__ correctly.
    exitcode = ex.returncode;
  } catch (...) {
    printf("Unknown exception, probably from the std lib");
    exitcode = EXIT_FAILURE;
  }
  log.close();
  reportStats();
  freeall(); //gratuitous, ending a process makes this moot, except for memory leak detector.
  return exitcode;
}

const char * Server::timetText() {
  thread_local Now imager(now()); //bridge while reconciling time types.Should implement writing to a FILE rather than passing the string to a writer.
  imager.format();
  return imager.image;
}

time_t Server::since(const NanoSeconds &lastActive) const {
  return epoller.elapsed-lastActive;
}
