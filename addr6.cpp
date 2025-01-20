
/**
// Created by andyh on 1/19/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#include "addr6.h"

#include <arpa/inet.h>

Inaddr6 &Inaddr6::clear() {
  for (auto index = 4; index-- > 0;) { //gcc converts this to a memset.
    __in6_u.__u6_addr32[index] = 0;
  }
  return *this;
}

bool Inaddr6::tripleZero() const {
  return __in6_u.__u6_addr32[0] == 0 && __in6_u.__u6_addr32[1] == 0 && __in6_u.__u6_addr32[2] == 0;
}

bool Inaddr6::isShort(uint32_t distinctive) const {
  return tripleZero() && __in6_u.__u6_addr32[3] == distinctive;
}

bool Inaddr6::isUnspecified() const {
  return isShort(0);
}

bool Inaddr6::isLoopback() const {
  return isShort(htonl(1));
}

bool Inaddr6::isLocal(uint32_t hostordered) const {
  return __in6_u.__u6_addr32[0] & htonl(0xffc00000) == htonl(hostordered);
}

bool Inaddr6::isLinkLocal() const {
  return isLocal(0xfe800000);
}

bool Inaddr6::isSiteLocal() const {
  return isLocal(0xfec00000);
}

bool Inaddr6::wasIpv4() const {
  return __in6_u.__u6_addr32[0] == 0 && __in6_u.__u6_addr32[1] == 0 && __in6_u.__u6_addr32[2] == htonl(0xffff);
}

bool Inaddr6::isV4compatible() const {
  return tripleZero() && ntohl(__in6_u.__u6_addr32[3]) > 1;
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
    return false;
  }
  return true;
}
