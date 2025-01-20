#pragma once
#include <netinet/in.h>
/**
// Created by andyh on 1/19/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/


/** add conveniences to an in6_addr **/
struct Inaddr6 : in6_addr {
  Inaddr6 &clear();

  bool tripleZero() const;

  bool isShort(uint32_t distinctive) const;

  bool isLocal(uint32_t hostordered) const;

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

struct SockAddr6 : sockaddr_in6 {
  Inaddr6 &addr6;
  SockAddr6() : addr6(*reinterpret_cast<Inaddr6 *>(&sin6_addr)) {}

  bool presentationToNetwork(const char *bindaddr);
};
