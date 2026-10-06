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
#include <cmsis-plus/diag/trace.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <sys/time.h>

#include <hw_result.hpp>
#include <test.h>

/*
 * Carried verbatim from the kernel's own suite, tests/sources/mutex-stress,
 * with two changes and no third.
 *
 *  - RUN_SECONDS defaults to 10 rather than 30. The runner passes no argv, and
 *    upstream's 30 s was chosen for a person watching the distribution
 *    converge, not for a suite.
 *  - The RESULT line and hw_result::ok()/fail() at the end, which is the
 *    verdict convention every test in this workspace follows and the only
 *    thing test_smpl/run-host.sh reads.
 *
 * WHY THIS TEST IS HERE. It is the port's OS_NCPU=1 leg. The nine SMP object
 * tests cannot be built single-core by construction -- without
 * OS_USE_SMP_SCHEDULER the kernel's `current_thread_` is a scalar and
 * `thread::cpu_affinity()` does not exist, and those tests use both -- so
 * something else has to exercise the port's non-SMP branch. Upstream's own
 * suite is the right thing to use rather than a test invented for the purpose,
 * and of the two candidates this is the one that is self-contained (the other,
 * rtos-apis, pulls in FatFs and posix-io).
 *
 * It is also apt: ten threads hammering one mutex is exactly the shape of the
 * defect this port found in os-mutex.cpp.
 */
#if !defined(RUN_SECONDS)
#define RUN_SECONDS (10)
#endif

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wc++98-compat"
#endif

using namespace os;
using namespace os::rtos;

#if defined(__ARM_EABI__)

void
busy_wait (unsigned int micros)
{
  clock::timestamp_t start = hrclock.now ();
  clock::timestamp_t until_cycles
      = start + hrclock.input_clock_frequency_hz () * micros / 1000000;

  clock::timestamp_t now_cycles;
  do
    {
      now_cycles = hrclock.now ();
    }
  while (now_cycles < until_cycles);
}

#else

void
busy_wait (unsigned int micros)
{
  /* struct */ timeval tp;
  gettimeofday (&tp, nullptr);
  uint64_t until_micros;
  until_micros
      = static_cast<uint64_t> (tp.tv_sec * 1000000 + tp.tv_usec) + micros;

  uint64_t now_micros;
  do
    {
      gettimeofday (&tp, nullptr);
      now_micros = static_cast<uint64_t> (tp.tv_sec * 1000000 + tp.tv_usec);
    }
  while (now_micros < until_micros);
}

#endif

#pragma GCC diagnostic push
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
int
os_main (int argc, char* argv[])
{
  unsigned int seconds = RUN_SECONDS;
  if (argc > 1)
    {
      seconds = static_cast<unsigned int> (atoi (argv[1]));
    }

  printf ("\nMutex stress & uniformity test\n");
#if defined(__clang__)
  printf ("Built with clang " __VERSION__ "\n");
#else
  printf ("Built with GCC " __VERSION__ "\n");
#endif

  uint32_t seed;

  int status;
  /* struct */ timeval tp;
  gettimeofday (&tp, nullptr);
  // Use some large prime numbers and the current time.
  // Must be a 32-bits value, to overflows and mess things further.
  seed = static_cast<uint32_t> ((tp.tv_sec + tp.tv_usec + 15485863)
                                * 179424673);

#pragma GCC diagnostic push
#if defined(__clang__)
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wuseless-cast"
#endif
  printf ("Seed %u\n", static_cast<unsigned int> (seed));
#pragma GCC diagnostic pop

  srand (seed);

  status = run_tests (seconds);

  printf ("\nRESULT: %s\n", (status == 0) ? "PASS" : "FAIL");
  if (status == 0)
    {
      hw_result::ok ();
    }
  hw_result::fail ();
}
#pragma GCC diagnostic pop
