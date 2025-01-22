/**
// Created by andyh on 1/13/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/

#include "darkhttpd.h"

#if DarklySupportForwarding
#warning "forwarding is supported"
#endif
#if DarklySupportDaemon
#warning "daemon operation is supported, including PID file generation"
#endif
#if DarklySuppportAcceptanceFilters
#warning "acceptance filters feature is enable"
#endif


int main(int argc, char *argv[]) {
  {//wrapped so that destructor gets called before we exit, so that we can look for memory leaks.
    DarkHttpd::Server server;
    server.main(argc,argv);
    //todo: implement interactive controls via fork, on child call server.main(argc,argv); then waitpid() on it while (p)polling() stdin, stdout.
    //cli functions: liststats, quit, restart (files changed) in fact add file change detect to main and let it wait until all current connections complete before prodding child to reload.
  }
  //here is where memory leaks can be reported
  return 0;
}

#if 0
Testing required:

multiple connections
mimetype lookups
forward_all
forward via virtual named hosts
directory listing
dots in urls, single, double, at end of names, in middle of names, dot files
authorization
index.html instead of directory listing
custom headers
keepalive
if modified since
why is user agent parsed?
HEAD for each GET and compare - returned should be same until the data comes.




#endif
