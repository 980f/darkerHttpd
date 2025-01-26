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
 * done: conditional compile for forwarding   DarklySupportForwarding
 * done: conditional compile for daemon       DarklySupportDaemon
 * todo: usage fragments in each module, paired with parameter settings.
 * todo: file offset and range logic is off by one. Will probably use half-open interval internal, modifying transmitted and parsed values which are fully closed.
 */

#pragma once

#include "byterange.h"
#include "darklogger.h"
#include "stringview.h"
#include "checkFormatArgs.h"
#include "dropprivilege.h"
#include "fd.h"
#include "mimer.h"
#include "now.h"


#include <vector>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <forward_list>


#include <netinet/in.h>
#include <sys/stat.h>

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
    Now last_active = 0;

    enum {
      BORN = 0, /* constructed, not fully initialized */
      RECV_REQUEST, /* receiving request */
      SEND_HEADER, /* sending generated header */
      SEND_REPLY, /* sending reply */
      DONE /* connection closed, need to remove from queue */
    } state = BORN; // DONE makes it harmless so it gets garbage-collected if it should, for some reason, fail to be correctly filled out.


    struct Request {
      //the following should be a runtime or at least compile time option. This code doesn't support put or post so it does not receive arbitrarily large requests.
      static constexpr size_t RequestSizeLimit = 1500; //vastly more than is needed for most GET'ing, this would only be small for a PUT and we will stream around the buffer by parsing as the content arrives and recognizing the PUT and the header boundary soon enough to do that.
      char theRequest[RequestSizeLimit + 1/*for null terminator */];
      StringView received{nullptr, 0, 0}; //bytes in.

      /* request fields */
      enum HttpMethods {
        NotMine = 0, GET, HEAD,
        // POST,PUT  //expose these only as we add code to support them.
      } method;

      StringView hostname; //this was missing in original server, that failed to do hostname checking that is nominally required by RFC7230
      StringView url;
      char *urlParams; //pointer to url text that followed a '?', 980f is considering using those in directory listings to support formatting options and the use a shell to ls for the listing.
      StringView referer;
      StringView user_agent;
      StringView authorization;
      bool is_https_redirect; //This just indicates protocol that the client says that they sent out, in case intervening layers strip that info. Its only use should be on outgoing redirection requests. Should be named 'redirect_is_https'
      Now if_mod_since;
      ByteRange range;

      struct Lifetime {
        bool dieNow = true;
        unsigned requested = 0;
        unsigned max = 0;

        bool timeToDie(time_t beenAlive);

        unsigned &cli_timeout;

        Lifetime(unsigned &cli_Timeout) : cli_timeout{cli_Timeout} {}
      } keepalive;

      void clear();

      Request(unsigned &cli_timeout);

      bool parse();

      /* call recv on the socket */
      ssize_t receive(int socket);
    } rq;

    struct Replier {
      int http_code = 0;
      bool header_only = false; //todo: this is ugly, should be in range of checking get vs head and content size.

      struct Block {
        Fd fd;
        // bool dont_free = false;
        //altering range begin rather than having a separate variable which usually was added to it dynamically  size_t sent = 0;
        ByteRange range; //tracks sending.


        void recycle(bool andForget);
        void recordSize() {
          range.setForSize(fd.getPosition());
        }

        void statSize() {
          range.setForSize(fd.getLength()); //we'll apply request range to this momentarily
        }

        FILE *createTemp();

        off_t getLength();

        /** @returns whether we have a good range and good fd
         * this is a key functionality, its logic must be based on httpd, not personal opinion of what makes a file good or bad.
         */
        bool operator!() {
          return !fd.seemsOk() || getLength()<0;
        }

        bool isRegularFile() {
          return fd.isRegularFile();
        }
      };

      Block header;
      Block content;

      void clear();
    } reply;

    /* header + body = total, for logging */
  public:
    void logOn(DarkLogger *log);

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

    void startCommonHeader(int errcode, const char *errtext, off_t contentLength);

    void catAuth();

    void catGeneratedOn(bool toReply);

    void endHeader();

    void startReply(int errcode, const char *errtext);

    void addFooter();

    void error_reply(int errcode, const char *errname, const char *format, ...) checkFargs(4, 5);

    void endReply();

    void redirect(const char *protocol, const char *hostpart, const char *uripart);

    void redirect_https();

    void process_get();

    void process_request();

    void poll_recv_request();

    int sendRange(Replier::Block &sending);

    void poll_send_header();

    void poll_send_reply();

    void generate_dir_listing(const char *path, const char *decoded_url);

    void urlDoDirectory();
  }; //end of connection child class

  class Server {
    friend Connection; //division of source into Server and Connection was done just to make more clear what is shared/configuration and what is per-request.
    /** until we use the full signal set ability to pass our object we only allow one DarkHttpd per process.*/
    static Server *forSignals; //epoll will let us eliminate this, it adds a user pointer to the notification structure.
    static void stop_running(int sig);

#ifdef HAVE_INET6
    bool inet6 = false; /* whether the socket uses inet6 */
#endif
    /* Defaults can be overridden on the command-line */
    const char *bindaddr = nullptr; //only assigned from cmdline
    uint16_t bindport; /* or 80 if running as root */

#if DarklySupportDaemon
    bool want_daemon = false;
#endif

    int max_connections = -1; /* kern.ipc.somaxconn */

    /* If a connection is idle for timeout_secs or more, it gets closed and
         * removed from the connlist.
         */
    unsigned timeout_secs = 30;
    bool want_keepalive = true;

    /** add pretty printer for local types. */
    struct ReallyDarkLogger : DarkLogger {
      void put(Connection::Request::HttpMethods method);
    } log;


    Mimer contentType;

    //todo: load only from file, not commandline. Might even drop feature.
    std::vector<const char *> custom_hdrs; //parse_commandline concatenation of argv's with formatting. Should just record their indexes and generate on sending rather than cacheing here.
#if DarklySupportAcceptanceFilter
    bool want_accf = false;//FreeBSD accept filter (replace runtime bitch with compile time flag and complaint when parsing command line/file
#endif

    bool want_server_id = true;
    StringView wwwroot = nullptr; /* a path name */ //argv[1]  and even if we demonize it is still present

    bool want_chroot = false;

    const char *index_name = "index.html";
    bool no_listing = false;

    DropPrivilege drop_uid{false};
    DropPrivilege drop_gid{true};

    /* name program was launched with, will eventually log it */
    char *invocationName;

  protected:
    Fd sockin; /* socket to accept connections from */
    bool accepting = true; /* set to 0 to stop accept()ing */
    volatile bool running = false; /* signal handler sets this to false */

    /** the entries will all be dynamically allocated */
    std::forward_list<Connection *> connections;

    Now now;

    void change_root();

    bool prepareToRun();

    void httpd_poll();

    void reportStats() const;

    void freeall();

    void init_sockin();

    void accept_connection();

    void log_connection(const Connection *conn);

  protected: //things connection can use

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

  public:
    Server() {
      forSignals = this;
    }

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
      StringView key = nullptr; //instead of expanding this we decode the incoming string.
      bool operator()(StringView authorization);

      operator bool() const {
        return key.notTrivial();
      }

      void operator=(char *arg) {
        //todo: check for ':' and split?
        key = arg; //todo: chase heritage and try to reduce key to StringView.
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


  template<typename Integrish> auto llu(Integrish x) {
    return static_cast<unsigned long long>(x);
  }
};
