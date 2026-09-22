/*
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) 2016-2025 Liviu Ionescu. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose is hereby granted, under the terms of the MIT license.
 *
 * If a copy of the license was not distributed with this file, it can be
 * obtained from https://opensource.org/licenses/mit.
 */

#include <cmsis-plus/rtos/os.h>
#include <hw_result.hpp>

#include <cstdio>

#include <test-cpp-api.h>
#include <test-c-api.h>
#include <test-iso-api.h>
#include <test-posix-io-api.h>
// #include <test-chan-fatfs.h>   // see the #if 0 below: not carried by this kernel

#include <test-cpp-mem.h>

#if defined(OS_USE_SEMIHOSTING_SYSCALLS)
#include <cmsis-plus/arm/semihosting.h>
#endif

// ----------------------------------------------------------------------------

int
os_main (int argc __attribute__ ((unused)),
         char* argv[] __attribute__ ((unused)))
{
  printf ("\n");
  printf ("µOS++ RTOS simple APIs test\n");
#if defined(__clang__)
  printf ("Built with clang " __VERSION__ "\n");
#else
  printf ("Built with GCC " __VERSION__ "\n");
#endif

  // fflush(stdout);

  int ret = 0;
  errno = 0;

#if 0
  if (ret == 0)
    {
      ret = test_cpp_mem ();
      printf ("errno=%d\n", errno);
      // fflush(stdout);
      errno = 0;
    }
#endif

#if 1
  if (ret == 0)
    {
      ret = test_cpp_api ();
      printf ("errno=%d\n", errno);
      // fflush(stdout);
      errno = 0;
    }
#endif

#if 1
  if (ret == 0)
    {
      ret = test_c_api ();
      printf ("errno=%d\n", errno);
      // fflush(stdout);
      errno = 0;
    }
#endif

#if 1
  if (ret == 0)
    {
      ret = test_iso_api (false);
      printf ("errno=%d\n", errno);
      // fflush(stdout);
      errno = 0;
    }
#endif

#if 1
  if (ret == 0)
    {
      ret = test_posix_io_api (false);
      printf ("errno=%d\n", errno);
      // fflush(stdout);
      errno = 0;
    }
#endif

/*
 * OFF, and not because of this port. chan-fatfs lives in upstream's separate
 * posix-io xpack (cmsis-plus/posix-io/chan-fatfs-file-system.h), which this
 * vendored kernel does not carry at all -- `find include src -name '*chan*'`
 * returns nothing. The sub-test would fail to compile on every port here, not
 * just on the host. Switched off in the file's own idiom, beside the
 * `#if 0` upstream already uses for test_cpp_mem.
 */
#if 0
  if (ret == 0)
    {
      ret = test_chan_fatfs (false);
      printf ("errno=%d\n", errno);
      // fflush(stdout);
      errno = 0;
    }
#endif

  printf ("done\n");

  /* The verdict, in the convention every test in this workspace follows and
     the only thing test_smpl/run-host.sh reads. hw_result flushes stdio
     before _exit(), which matters here: this test prints with printf(). */
  printf ("\nRESULT: %s\n", (ret == 0) ? "PASS" : "FAIL");
  if (ret == 0)
    {
      hw_result::ok ();
    }
  hw_result::fail ();
}

// ----------------------------------------------------------------------------
