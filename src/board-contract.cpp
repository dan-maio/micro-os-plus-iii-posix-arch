/*
 * board-contract.cpp - no code. It fails the build the moment a board has
 * not stated something the shared sources need.
 *
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 *
 * The same file exists in the three cross-compiled projects and exists here
 * for the same reason: a board that leaves something unset must produce a
 * compile error naming the board and the thing, not a silent inheritance of
 * another board's answer -- or, worse, a link that succeeds and a test that
 * proves nothing.
 */

#if defined(__APPLE__) || defined(__linux__)

#include <cmsis-plus/rtos/port/os-decls.h>

#if !defined(OS_NCPU)
#error "The board does not set OS_NCPU."
#endif

#if OS_NCPU < 1
#error "OS_NCPU must be at least 1."
#endif

#if !defined(PORT_GREETING)
#error "The board does not define PORT_GREETING; the test banners print it."
#endif

// A host CPU is a host thread, so OS_NCPU above the number the machine can
// actually run in parallel is not an error -- it oversubscribes, which is a
// legitimate and useful way to shake out races. It IS worth saying out loud,
// because a suite that suddenly takes ten times as long has usually done this
// by accident.
#if OS_NCPU > 16
#warning "OS_NCPU above 16 oversubscribes almost any host; expect the tick to drift."
#endif

#endif /* defined(__APPLE__) || defined(__linux__) */
