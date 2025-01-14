/**
// Created by andyh on 12/14/24.
// Copyright (c) 2024 Andy Heilveil, (github/980f). All rights reserved.
*/

/* This module was started using C source whose license was:
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
 */

#pragma once

#ifdef __linux
#ifndef _GNU_SOURCE // suppress warning, not sure who already set this.
#define _GNU_SOURCE /* for strsignal() and vasprintf() */
#endif
#endif

#include <cheaptricks.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <forward_list>
#include <locale>
#include <map>
#include <stdarg.h>
#include <netinet/in.h>
#include <unistd.h>
#include <vector>

/** build options */
#ifndef NO_IPV6
#define HAVE_INET6
#endif

// gizmo to get the compiler to check our printf args:
#ifndef checkFargs
#ifdef __GNUC__
/* [->] borrowed from FreeBSD's src/sys/sys/cdefs.h,v 1.102.2.2.2.1 */
#define checkFargs(fmtarg, firstvararg) \
  __attribute__((__format__(__printf__, fmtarg, firstvararg)))
/* [<-] */
#else
#define checkFargs(fmtarg, firstvararg)
#endif
#endif


/** to help ensure timely frees.
 * Many people seem to not know that 'free' doesn't mind null pointers, it checks for them and does nothing so that we don't have to do that in a gazillion places.
 *
 * This frees what it has been given at constructor or by assignment when there is an assignment or a destruction. As such never assign to it from anything not malloc'd.
 */
struct AutoString {
  //these are exposed because we are mutating C code into C++ and hiding them is tedious. We should hide at least the pointer member, but there are a few contexts (*printf) where the implicit conversion doesn't get invoked.
  char *pointer = nullptr;

  char &operator[](off_t offset) const {
    static char fakechar;
    if (!pointer) {
      return fakechar;
    }
    return pointer[offset];
  }

  size_t length = 0;

  operator char *() const {
    return pointer;
  }

  bool notTrivial() const {
    return pointer && length > 0;
  }

  /** @returns whether we have a nontrivial block*/
  operator bool() const {
    return notTrivial();
  }

  bool operator!() const {
    return pointer == nullptr || length == 0;
  }

  /**
   * @param str is this what the string ends with?
   * @param len length if known, else leave off or pass ~0 and strlen will be called on str
   * @returns whether the internal string ends with str .
   */
  bool endsWith(const char *str, unsigned len = ~0) const;

  /** @returns whether last char is @param slash*/
  bool endsWith(char slash) {
    return pointer && length > 0 && slash == pointer[length -1];
  }

  /**
   * @param str to append to end after reallocating room for it
   * @param len length of string if known, ~0 for unknown (default)
   * @returns whether the append happened, won't if out of heap
   */
  bool cat(const char *str, size_t len = ~0);

  /** construct around an allocated content. */
  AutoString(char *pointer = nullptr, unsigned length = ~0) : pointer(pointer), length(length) {
    if (pointer && length == ~0) {
      this->length = strlen(pointer);
    }
  }

  void Free() {
    free(pointer);
    // null fields in case someone tries to use this after it is deleted
    pointer = nullptr;
    length = 0;
  }

  ~AutoString() {
    Free();
  }

  AutoString &operator=(AutoString &other);

  AutoString &operator=(char *replacement);

  AutoString &toUpper();

  /** frees present content and mallocs @param amount bytes plus one for a null that it inserts. */
  bool malloc(size_t amount);

  AutoString(AutoString &&other) = delete;

  unsigned int catf( const char *format, ...) {

    return length;
  }

};

/** a not-null terminated string that we can't alter through this object. This class exists so that we can sub-string without touching the orignal string OR copying it.*/

struct StringView {
  const char *pointer = nullptr;
  size_t length = 0;
  size_t start = 0;

  StringView(const char *pointer = nullptr, size_t length = ~0, size_t start = 0);

  StringView &operator=(const char *str);

  /** @returns pointer to byte after where this StringView's content was inserted. If this guy is trivial then this will be the same as @param bigenough.
   * @param honorNull is whether to stop inserting this guy if a null is found short of the length.
   */
  char *put(char *bigenough, bool honorNull = true) const;

  /** calls @see put then adds a terminator and returns a pointer to that terminator. */
  char *cat(char *bigenough, bool honorNull = true) const;

  size_t put(FILE *out) const;

  const char *begin() const {
    return pointer + start;
  }

  void trimTrailing(const char *trailers);

  AutoString clone() const {
    char *clone = static_cast<char *>(malloc(length + 1));
    if (clone == nullptr) {
      return AutoString();
    }
    memcpy(clone, begin(), length);
    clone[length] = 0;
    return AutoString(clone, length);
  }
};

/** add conveniences to an in6_addr **/
struct Inaddr6 : in6_addr {
  Inaddr6 &clear();

  Inaddr6() : in6_addr() {
    clear();
  }

  bool isUnspecified() const;

  bool isLoopback() const;

  bool isLinkLocal() const;

  bool isSiteLocal() const;

  /** @returns whether this ipv6 address is a mapped ipv4*/
  bool wasIpv4() const;

  bool isV4compatible() const;

  bool operator ==(const Inaddr6 &other) const;

  bool isMulticast() const;
};

struct SockAddr6:sockaddr_in6 {
  Inaddr6 &addr6 ;
  SockAddr6() : addr6(*reinterpret_cast<Inaddr6*>(&sin6_addr)) {}

  bool presentationToNetwork(const char *bindaddr);
};

class DarkException;

class DarkHttpd {
  /** until we use the full signal set ability to pass our object we only allow one DarkHttpd per process.*/
  static DarkHttpd *forSignals; // trusting BSS zero to clear this.

  DarkHttpd() {
    forSignals = this;
  }

  /** todo: this seems to have become the same as mime_mapping except for idiot checks */
  struct forward_mapping : std::map<const char *, const char *> {
    // const char *host, *target_url; /* These point at argv. */
    void add(const char *const host, const char *const target_url) {
      insert_or_assign(host, target_url); // # allows breakpoint on these. We need to decide whether multiples are allowed for the same host, at present that has been excluded but the orignal might have allowed for that in which case we need a map of list of string.
    }

    /** free contents, then forget them.*/
    void purge() {
      for (auto each: *this) {
        free((void *) each.first);
        free((void *) each.second);
      }
      clear();
    }
  } forward_map;

public:
  class Fd { // a minimal one compared to safely/posix
  protected:
    int fd = -1;
    FILE *stream=nullptr;

  public:
    FILE *getStream() {//this "create at need"
      if (fd==-1) {
        return nullptr;//todo: or perhaps spew to stderr?? This will likely segv.
      }
      if (stream == nullptr) {
        stream=fdopen(fd,"wb");//using b as we must use network line endings, not the platform's idea of them
      }
      return stream;
    }

  public:
    // ReSharper disable once CppNonExplicitConversionOperator
    operator int() const {
      return fd;
    }

    bool seemsOk() {
      return fd != -1;
    }

    int operator=(int newfd) { // NOLINT(*-unconventional-assign-operator)
      fd = newfd;
      //consider caching stream here, if fd is open.
      return newfd;
    }

    bool operator==(int newfd) const {
      return fd == newfd;
    }

    // ReSharper disable once CppNonExplicitConversionOperator
    operator bool() const {
      return fd != -1;
    }

    /** @returns whether close() thinks it worked , but we discard our fd value regardless of that */
    bool close() {
      if (seemsOk()) {
        int fi = ::close(fd);
        fd = -1;
        return fi != -1;
      }
      return true; // already closed is same as just now successfully closed.
    }

    /** lose track of the fd regardless of the associated file's state. Most uses seem like bugs, leaks of OS file handles.*/
    void forget() {
      fd = -1;
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

    Fd() = default;

    // ReSharper disable once CppNonExplicitConvertingConstructor
    Fd(int open) : fd(open) {}

    size_t printf(const char*format ,...);
  };


  struct Connection {
    Fd socket;
    DarkHttpd &service;
#ifdef HAVE_INET6
    in6_addr client;
#else
    in_addr_t client;
#endif
    time_t last_active = 0;

    enum {
      RECV_REQUEST, /* receiving request */
      SEND_HEADER, /* sending generated header */
      SEND_REPLY, /* sending reply */
      DONE /* connection closed, need to remove from queue */
    } state = DONE; // DONE makes it harmless so it gets garbage-collected if it should, for some reason, fail to be correctly filled out.

    /* char request[request_length+1] is null-terminated */
    AutoString request;

    /* request fields */
    AutoString method; // as in GET, POST, ...  //sub_string(request)
    AutoString url; //sub_string(request)
    AutoString referer; //parse_field
    AutoString user_agent; //parse_field
    AutoString authorization; //parse_field

    //should structure this group, values come from parse_field
    off_t range_begin = 0;
    off_t range_end = 0;
    bool range_begin_given = false;
    bool range_end_given = false;

    AutoString header; //outgoing message, could replace with generator rather than cat strings together then sending block, in fact could fprintf to a temp file then netcat that file.

    size_t header_sent = 0;
    bool header_dont_free = false;
    bool header_only = false;
    int http_code = 0;
    bool conn_closed = true;

    enum {
      REPLY_GENERATED,
      REPLY_FROMFILE,
      REPLY_NONE
    } reply_type = REPLY_NONE;

    Fd header_fd;

    AutoString reply = nullptr;//wil be replacing this with creating a temp file and then always sending header as a file then reply as a file. Later on we'll figure out how to merge them when the reply is internally generated.
    bool reply_dont_free = false;
    Fd reply_fd;
    off_t reply_start = 0;
    off_t file_length = 0;
    off_t reply_sent = 0;
    off_t total_sent = 0;
    /* header + body = total, for logging */
  public:
    Connection(DarkHttpd &parent,int fd);//only called via new in socket acceptor code.

    void clear();

    void recycle();

    void poll_check_timeout();

    const char *keep_alive() const;

    void startHeader(int errcode, const char *errtext);

    void catDate();

    void catServer();

    void catFixed(const char *fixedText);

    void catKeepAlive();

    void catCustomHeaders();

    void catContentLength(off_t off);

    void startCommonHeader(int errcode,const char *errtext,off_t contentLenght);

    void catAuth();

    void catGeneratedOn(bool toReply);

    void endHeader();

    void startReply(int errcode, const char *errtext);

    void addFooter();

    void default_reply(int errcode, const char *errname, const char *format, ...) checkFargs(4, 5);

    void redirect(const char *format, ...) checkFargs(2, 3);

    char *parse_field(const char *field) const;

    void redirect_https();

    bool is_https_redirect() const;

    void parse_range_field();

    bool parse_request();

    void process_get();

    void process_request();

    void poll_recv_request();

    void poll_send_header();

    void poll_send_reply();

    void generate_dir_listing(const char *path, const char *decoded_url) ;

  };//end of connection child class

  void load_mime_map_file(const char *filename);

  StringView url_content_type(const char *url);

  const char *get_address_text(const void *addr) const;

  void init_sockin();

  void usage(const char *argv0);

  void httpd_poll();

  bool daemonize_start();

  void daemonize_finish();

  void freeall();

  bool parse_commandline(int argc, char *argv[]);

  void accept_connection();

  void log_connection(const Connection *conn);

  void prepareToRun();

  void change_root();

#define DATE_LEN 30 /* strlen("Fri, 28 Feb 2003 00:02:08 GMT")+1 */

  const char *generated_on(const char date[DATE_LEN]) const;

  static void stop_running(int sig);

  void reportStats() const;

  int main(int argc, char **argv);

private:
  Fd fd_null;

  struct PipePair {
    Fd fds[2];

    int operator[](bool which) {
      return fds[which];
    }

    bool connect() {
      int punned[2] = {fds[0], fds[1]};
      return pipe(punned) != -1;
    }

    void close();
  } lifeline;


  Fd sockin; /* socket to accept connections from */

  /** the entries will all be dynamically allocated */
  std::forward_list<Connection *> entries;

  const char *forward_all_url = nullptr;

  bool forward_to_https = false;

  /* If a connection is idle for timeout_secs or more, it gets closed and
   * removed from the connlist.
   */
  unsigned timeout_secs = 30;
  AutoString keep_alive_field; //xasprintf but only ever filled from that one instance.

  /* Time is cached in the event loop to avoid making an excessive number of
   * gettimeofday() calls.
   */
  struct Now {
  private:
    time_t raw=0;
  public:
    operator time_t() {
      return raw;
    }
    time_t utc;
    char image[DATE_LEN];
    void refresh() {
      raw = time(&utc);//twofer: both are set to the same value
      //rfc11whatever format.
      image[strftime(image, DATE_LEN, "%a, %d %b %Y %H:%M:%S GMT", gmtime(&utc))] = 0; // strftime returns number of chars it put into dest.
    }
  } now ;

  /* To prevent a malformed request from eating up too much memory, die once the
   * request exceeds this many bytes:
   */
#define MAX_REQUEST_LENGTH 4000

  /* Defaults can be overridden on the command-line */
  const char *bindaddr = nullptr; //only assigned from cmdline
  uint16_t bindport; /* or 80 if running as root */
  int max_connections = -1; /* kern.ipc.somaxconn */
  const char *index_name = "index.html";
  bool no_listing = false;

#ifdef HAVE_INET6
  bool inet6 = false; /* whether the socket uses inet6 */
#endif
  StringView default_mimetype{"application/octet-stream"}; //an argv or builtin constant
  /** file of mime mappings gets read into here.*/
  AutoString mimeFileContent;
  StringView wwwroot; /* a path name */ //argv[1]  and even if we demonize it is still present

  char *logfile_name = nullptr; /* NULL = no logging */
  FILE *logfile = nullptr;

  /* [->] pidfile helpers, based on FreeBSD src/lib/libutil/pidfile.c,v 1.3
   * Original was copyright (c) 2005 Pawel Jakub Dawidek <pjd@FreeBSD.org>
   */
  struct PidFiler:Fd {
    char *file_name = nullptr; /* NULL = no pidfile */

    void remove();

    void Remove(const char *why);

    int file_read();

    void create();


  } pid;

  bool want_chroot = false;
  bool want_daemon = false;
  bool want_accf = false;
  bool want_keepalive = true;
  bool want_server_id = true;
  bool want_single_file = false;

  AutoString server_hdr; //pkgname in building of replys //todo: replace with conditional sending of constant.
  AutoString auth_key; /* NULL or "Basic base64_of_password" */ //base64 expansion of a cli arg.
  //todo: load only from file, not commandline. Might even drop feature. Alternative is a std::vector of headers rather than a blob.
  std::vector<const char *> custom_hdrs; //parse_commandline concatenation of argv's with formatting. Should just record their indexes and generate on sending rather than cacheing here.

  /** things that are interesting but don't affect operation */
  struct Fyi {
    uint64_t num_requests = 0;
    uint64_t total_in = 0;
    uint64_t total_out = 0;
  } fyi;

  bool accepting = true; /* set to 0 to stop accept()ing */
  bool syslog_enabled = false;
  volatile bool running = false; /* signal handler sets this to false */

#define INVALID_UID ((uid_t) ~0)
#define INVALID_GID ((gid_t) ~0)

  uid_t drop_uid = INVALID_UID;
  gid_t drop_gid = INVALID_GID;
};
