/*
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) 2016-2025 Liviu Ionescu. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 *
 * If a copy of the license was not distributed with this file, it can
 * be obtained from https://opensource.org/licenses/mit/.
 */

/*
 * This file is part of the CMSIS++ proposal, intended as a CMSIS
 * replacement for C++ applications.
 *
 * If contains the scheduler implementation that uses
 * functions from the POSIX API (macOS and GNU/Linux).
 *
 * This file is included in all src/os-*.cpp files.
 */

#ifndef CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_
#define CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_

// ----------------------------------------------------------------------------

#if defined(OS_USE_OS_APP_CONFIG_H)
#include <cmsis-plus/os-app-config.h>
#endif

#include <cmsis-plus/rtos/os-c-decls.h>

// ----------------------------------------------------------------------------

#ifdef __cplusplus

// ----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/time.h>

// For Linux
#include <unistd.h>

#include <cmsis-plus/diag/trace.h>

// ----------------------------------------------------------------------------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wc++98-compat"
#endif

// ----------------------------------------------------------------------------

extern "C" uint32_t signal_nesting;

namespace os
{
  namespace rtos
  {
    namespace port
    {
      // ----------------------------------------------------------------------

      namespace scheduler
      {

        inline port::scheduler::state_t __attribute__ ((always_inline))
        lock (void)
        {
          return locked (state::locked);
        }

        inline port::scheduler::state_t __attribute__ ((always_inline))
        unlock (void)
        {
          return locked (state::unlocked);
        }

        inline bool __attribute__ ((always_inline))
        locked (void)
        {
          return lock_state != state::unlocked;
        }

        inline void __attribute__ ((always_inline))
        wait_for_interrupt (void)
        {
#if defined(OS_TRACE_RTOS_THREAD_CONTEXT)
          trace::printf ("%s() \n", __func__);
#endif
          pause ();
        }

      } /* namespace scheduler */

      namespace interrupts
      {

        inline bool __attribute__ ((always_inline))
        in_handler_mode (void)
        {
          return (signal_nesting > 0);
        }

        inline bool __attribute__ ((always_inline))
        is_priority_valid (void)
        {
          return true;
        }

      } /* namespace interrupts */

      // ======================================================================

      namespace this_thread
      {
        inline void __attribute__ ((always_inline))
        prepare_suspend (void)
        {
        }

      } /* namespace this_thread */

      // ======================================================================

      inline constexpr bool __attribute__ ((always_inline))
      clock_highres::has_hardware_counter (void) noexcept
      {
        return true;
      }

      inline uint64_t __attribute__ ((always_inline))
      clock_highres::hardware_counter (void) noexcept
      {
        timespec tp;   // NB: no 'struct' — baseline gate has -Werror=redundant-tags
        ::clock_gettime (CLOCK_MONOTONIC, &tp);
        return static_cast<uint64_t> (tp.tv_sec) * 1000000ULL
               + static_cast<uint64_t> (tp.tv_nsec) / 1000ULL;
      }

    } /* namespace port */
  } /* namespace rtos */
} /* namespace os */

#pragma GCC diagnostic pop

// ----------------------------------------------------------------------------

#endif /* __cplusplus */

// ----------------------------------------------------------------------------

#endif /* CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_ */
