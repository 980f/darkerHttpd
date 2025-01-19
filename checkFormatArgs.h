#pragma once
/**
// Created by andyh on 1/19/25.
// Copyright (c) 2025 Andy Heilveil, (github/980f). All rights reserved.
*/


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
