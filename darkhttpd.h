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
 * removed "single file" concept, use a directory with just the one file in it, via a file link, rather than having lots of code in this program for such a rare occurrence. A file glob filter would be a better feature.
 * todo: conditional compile for forwarding   DarklySupportForwarding
 * todo: conditional compile for daemon       DarklySupportDaemon
 * todo: usage fragments in each module, paired with parameter settings.
 */

#pragma once

#ifdef __linux
#ifndef _GNU_SOURCE // suppress warning, not sure who already set this.
#define _GNU_SOURCE /* for strsignal() and vasprintf() */
#endif
#endif

#include <byterange.h>

#include "stringview.h"
#include "autostring.h"
#include "checkFormatArgs.h"
#include "fd.h"

#include <vector>
#include <cstring>
#include <fcntl.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <unistd.h>
#include <forward_list>

#include <locale>
#include <map>

#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/syslog.h>

/** build options */
#ifndef NO_IPV6
#define HAVE_INET6
#endif

//FreeBSD can support acceptance filters, #define DarklySupportAcceptanceFilter 0 to disable compilation regardless of platform.
#ifndef DarklySupportAcceptanceFilter
#ifdef __FreeBSD__
#define DarklySupportAcceptanceFilter true
#endif
#endif

class DarkException;

namespace DarkHttpd {
  class Server; //server and connection know about each other. Can subclass the shared part and have a clean hierarchy.

  struct Connection {
    Fd socket;
    Server &service;
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

    //the following should be a runtime or at least compile time option. This code doesn't support put or post so it does not receive arbitrarily large requests.
    static constexpr size_t RequestSizeLimit = 1023; //vastly more than is needed for most GET'ing, this would only be small for a PUT and we will stream around the buffer by parsing as the content arrives and recognizing the PUT and the header boundary soon enough to do that.
    char theRequest[RequestSizeLimit];
    StringView received{theRequest, 0, 0}; //bytes in.
    StringView parsed{theRequest, 0, 0}; //the part of the request that has been parsed so far.

    // struct Header {
    //   const char *const name;
    //   StringView value;
    //
    //   void clear() {
    //     value.length = 0;
    //   }
    //
    //   Header(const char *name): name(name), value{nullptr} {}
    // };

    /* request fields */
    enum HttpMethods {
      NotMine = 0, GET, HEAD,
      // POST,PUT  //expose these only as we add code to support them.
    } method;

    StringView hostname; //this was missing in original server, that failed to do hostname checking that is nominally required by RFC7230
    StringView url; //sub_string(request)
    char *urlParams; //pointer to url text that followed a '?', 980f is considering using those in directory listings to support formatting options and the use a shell to ls for the listing.
    StringView referer; //parse_field
    StringView user_agent; //parse_field
    StringView authorization; //parse_field
    StringView if_mod_since;

    ByteRange range;

    bool conn_closed = true; //move this back to connection itself.

    struct Replier {
      int http_code = 0;
      bool header_only = false;

      struct Block {
        Fd fd;
        bool dont_free = false;
        size_t sent = 0;

        void recycle(bool andForget) {
          dont_free = false;
          sent = 0;
          if (andForget) { //suspicious fragment in the original, abandoned an open file descriptor, potentially leaking it.
            fd.forget(); // but it might be still open ?!
          }
          fd.close();
        }

        void clear() {
          if (!dont_free) {
            fd.unlink();
          }
        }
      };

      Block header;

      Block content;
      // Fd content_fd;
      // bool dont_free = false;
      // off_t sent = 0;
      //
      off_t start = 0;
      off_t file_length = 0;
      off_t total_sent = 0;

      void recycle();

      off_t getContentLength() {
        return content.fd.getLength();
      }
    } reply;

    /* header + body = total, for logging */
  public:
    Connection(Server &parent, int fd); //only called via new in socket acceptor code.

    /* forget the past request, and everything parsed from it or generated for it */
    void clear();

    void recycle();

    void poll_check_timeout();

    void startHeader(int errcode, const char *errtext);

    void catDate();

    void catServer();

    void catFixed(const char *fixedText);

    void catKeepAlive();

    void catCustomHeaders();

    void catContentLength(off_t off);

    void startCommonHeader(int errcode, const char *errtext, off_t contentLenght);

    void catAuth();

    void catGeneratedOn(bool toReply);

    void endHeader();

    void startReply(int errcode, const char *errtext);

    void addFooter();

    void error_reply(int errcode, const char *errname, const char *format, ...) checkFargs(4, 5);

    void endReply();

    void redirect(const char *protocol, const char *hostpart, const char *uripart);

    void redirect_https();

    bool is_https_redirect; //todo: usage seems bonkers, need to check original code. This just indicates protocol that the client sent out, in case intervening layers stripped that info. Its only use should be on outgoing requests.

    bool parse_request();

    void process_get();

    void process_request();

    void poll_recv_request();

    void poll_send_header();

    void poll_send_reply();

    void generate_dir_listing(const char *path, const char *decoded_url);
  }; //end of connection child class

  class Server {
    friend Connection;
    /** until we use the full signal set ability to pass our object we only allow one DarkHttpd per process.*/
    static Server *forSignals; //epoll will let us eliminate this, it adds a user pointer to the notification structure.
    static void stop_running(int sig);

#ifdef HAVE_INET6
    bool inet6 = false; /* whether the socket uses inet6 */
#endif
    /* Defaults can be overridden on the command-line */
    const char *bindaddr = nullptr; //only assigned from cmdline
    uint16_t bindport; /* or 80 if running as root */
    int max_connections = -1; /* kern.ipc.somaxconn */
    bool want_daemon = false;

    struct Logger {
      bool syslog_enabled = false;
      char *file_name = nullptr; /* NULL = no logging */
      FILE *file = nullptr;

      bool begin();

      void close();

      // /* Should this character be logencoded?
      //  */
      // static bool needs_encoding(const unsigned char c) {
      //   return ((c <= 0x1F) || (c >= 0x7F) || (c == '"'));
      // }
      //
      // /* Encode string for logging.
      //  */
      // static void logencode(const char *src, char *dest) {
      //   static const char hex[] = "0123456789ABCDEF";
      //   int i, j;
      //
      //   for (i = j = 0; src[i] != '\0'; i++) {
      //     if (needs_encoding((unsigned char) src[i])) {
      //       dest[j++] = '%';
      //       dest[j++] = hex[(src[i] >> 4) & 0xF];
      //       dest[j++] = hex[src[i] & 0xF];
      //     } else {
      //       dest[j++] = src[i];
      //     }
      //   }
      //   dest[j] = '\0';
      // }
      // template<typename Anything> void put(Anything thing);


      void put(char &&item) {
        if (syslog_enabled) {
          syslog(LOG_INFO,"%c",item);
        } else if (file) {
          fputc(item, file);
        }
      }

      void put(const char * &&item) {
        if (syslog_enabled) {
          syslog(LOG_INFO,"%s",item);
        } else if (file) {
          fputs(item, file);
        }
      }

      void put(time_t &&time) {
#define CLF_DATE_LEN 29 /* strlen("[10/Oct/2000:13:55:36 -0700]")+1 */
          char dest[CLF_DATE_LEN];
          tm tm;
          localtime_r(&time, &tm);
          if (strftime(dest, CLF_DATE_LEN, "[%d/%b/%Y:%H:%M:%S %z]", &tm) == 0) {
            dest[0] = 0;
          }
          put(dest);

#undef CLF_DATE_LEN
      }

      void put(Connection::HttpMethods method) {
        switch (method) {
          case Connection::GET:
            put("GET");
            break;
          case Connection::HEAD:
            put("HEAD");
            break;
          default:
            put("Unknown");
            break;
        }
      }

      void put(const StringView &view) {
        if (syslog_enabled) {
          syslog(LOG_INFO,"%*s", int(view.length), view.begin());
        } else if (file) {
          fprintf(file, "%*s", int(view.length), view.begin());
        }
      }

      /* todo: this guy might be intercepting and truncating longer int types. But we already expect to choke on 2Gig+ files so fixing this is not urgent */
      void put(const int &number) {
        if (syslog_enabled) {
          syslog(LOG_INFO,"%d", number);
        } else if (file) {
          fprintf(file, "%d", number);
        }
      }

      /* list processor template magic. Add a put() variation for any type that needs to be emitted */
      template<typename First, typename... Rest> void tsv(First &&first, Rest &&... rest) {
        put(std::forward<First>(first));
        if constexpr (sizeof...(rest) > 0) {
          put('\t');
          tsv(std::forward<Rest>(rest)...);
        } else {
          put('\n');
        }
      }
    } log;

    /* If a connection is idle for timeout_secs or more, it gets closed and
         * removed from the connlist.
         */
    unsigned timeout_secs = 30;


    const char *default_mimetype = nullptr;
    /** file of mime mappings gets read into here.*/
    AutoString mimeFileContent;
    /** file to load mime types map from */
    char *mimeFileName;

    //todo: load only from file, not commandline. Might even drop feature.
    std::vector<const char *> custom_hdrs; //parse_commandline concatenation of argv's with formatting. Should just record their indexes and generate on sending rather than cacheing here.
#if DarklySupportAcceptanceFilter
    bool want_accf = false;//FreeBSD accept filter (replace runtime bitch with compile time flag and complaint when parsing command line/file
#endif

    bool want_keepalive = true;
    bool want_server_id = true;
    StringView wwwroot; /* a path name */ //argv[1]  and even if we demonize it is still present

    bool want_chroot = false;

    const char *index_name = "index.html";
    bool no_listing = false;

#define INVALID_UID ((uid_t) ~0)
#define INVALID_GID ((gid_t) ~0)

    struct DropPrivilege {
      bool asGgroup;
      char *byName = nullptr;
      unsigned byNumber;

      bool operator !() const {
        return byName != nullptr;
      }

      operator unsigned() const {
        return byNumber;
      }

      const char *typeName();

      bool validate();

      void operator=(char *arg) {
        byName = arg;
        //maybe: validate()
        byNumber = atoi(arg); //unchecked conversion
      }

      bool operator()();
    };

    DropPrivilege drop_uid{false};
    DropPrivilege drop_gid{true};

    /* name program was launched with, will eventually log it */
    char *invocationName;

  protected:
    Fd sockin; /* socket to accept connections from */
    /** the entries will all be dynamically allocated */
    std::forward_list<Connection *> connections;

    bool accepting = true; /* set to 0 to stop accept()ing */

    volatile bool running = false; /* signal handler sets this to false */

    void change_root();

    bool prepareToRun();

    void httpd_poll();

    void reportStats() const;

    void freeall();

    void load_mime_map_file(const char *filename);

    void init_sockin();

    void accept_connection();

    void log_connection(const Connection *conn);

  protected: //things connection can use
    const char *url_content_type(const char *url);

    const char *get_address_text(const void *addr) const;

#if DarklySupportDaemon
    struct Daemon {
      struct PipePair {
        Fd fds[2];

        int operator[](bool which) const {
          return fds[which];
        }

        bool connect() const {
          int punned[2] = {fds[0], fds[1]};
          return pipe(punned) != -1;
        }

        void close();
      } lifeline;

      Fd fd_null;
      /* [->] pidfile helpers, based on FreeBSD src/lib/libutil/pidfile.c,v 1.3
       * Original was copyright (c) 2005 Pawel Jakub Dawidek <pjd@FreeBSD.org>
       */
      struct PidFiler : Fd {
        char *file_name = nullptr; /* NULL = no pidfile */

        void remove();

        void Remove(const char *why);

        bool file_read();

        void create();

        pid_t lastWrote = 0;

        char lastRead[8]; //4 meg is the largest of any OS of interest for the foreseeable future, we are not going to run this code on a super computer cluster.
      } pid;

      bool start();

      void finish();
    } d;

#endif

    // const char *generated_on(const char date[DATE_LEN]) const;

  public:
    Server(): mimeFileContent(nullptr), wwwroot{nullptr} {
      forSignals = this;
    }

    /** time of latest event, in 3 formats. */
    struct Now {
#define DATE_LEN 30 /* strlen("Fri, 28 Feb 2003 00:02:08 GMT")+1 */

    private:
      time_t raw = 0;

    public:
      operator time_t() const {
        return raw;
      }

      time_t utc;
      char image[DATE_LEN];

      void refresh() {
        raw = time(&utc); //twofer: both are set to the same value
        //rfc11whatever format.
        image[strftime(image, DATE_LEN, "%a, %d %b %Y %H:%M:%S GMT", gmtime(&utc))] = 0; // strftime returns number of chars it put into dest.
      }
    } now;

  private:
#if DarklySupportForwarding
    struct Forwarding {
      const char *all_url = nullptr;

      bool to_https = false;

      /** todo: replace with listmaker base class as linear search is more than fast enough. In fact, just do a string search after replacing cli args with file read.*/
      struct forward_mapping : std::map<const char *, const char *> {
        // const char *host, *target_url; /* These point at argv. */
        void add(const char *const host, const char *const target_url) {
          insert_or_assign(host, target_url); // # allows breakpoint on these. We need to decide whether multiples are allowed for the same host, at present that has been excluded but the orignal might have allowed for that in which case we need a map of list of string.
        }

        /** free contents, then forget them.*/
        void purge() {
          for (auto each: *this) {
            free(const_cast<char *>(each.first));
            free(const_cast<char *>(each.second));
          }
          clear();
        }
      } map;

      const char *operator()(StringView hostname) {
        /* test the host against web forward options */
        if (map.size() > 0) {
          if (hostname.notTrivial()) {
            // debug("host=\"%s\"\n", hostname.pointer);
            auto forward_to = map.find(hostname);
            if (forward_to != map.end()) {
              return forward_to->second;
            }
          }
        }
        return all_url;
      }
    } forward;
#endif

  protected:
    struct Authorizer {
      AutoString key; /* NULL or "Basic base64_of_password" */ //base64 expansion of a cli arg.
      bool operator()(StringView authorization);

      void operator=(char *arg) {
        //todo: check for ':' and split?
        key = strdup(arg); //todo: chase heritage and try to reduce key to StringView.
      }
    } auth;

    /** things that are interesting but don't affect operation */
    struct Fyi {
      uint64_t num_requests = 0;
      uint64_t total_in = 0;
      uint64_t total_out = 0;
    } fyi;

  public:
    void usage(const char *argv0);

    bool parse_commandline(int argc, char *argv[]);

    int main(int argc, char **argv);
  };
};
