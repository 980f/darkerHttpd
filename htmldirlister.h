
/**
// Created by andyh on 1/23/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#pragma once
#include <darkhttpd.h>
#include <directorylisting.h>
#include <fd.h>

using namespace DarkHttpd;

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

    conn.startCommonHeader(200, "OK", conn.reply.content.getLength());
    conn.catFixed("Content-Type: text/html; charset=UTF-8\r\n");
    conn.endHeader();
  }
};
