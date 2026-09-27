/*
 * os-inlines.h - the hot half of the POSIX port: masking and the kernel lock.
 *
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) 2016-2025 Liviu Ionescu. All rights reserved.
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 *
 * Read this beside micro-os-plus-iii-aarch64/include/cmsis-plus/rtos/port/
 * os-inlines.h. The two files have the same shape function for function; only
 * the mechanism differs, and that is the whole point of the port seam:
 *
 *   concern        AArch64                    here
 *   ------------------------------------------------------------------------
 *   CPU id         MRS MPIDR_EL1, & 3         thread_local, set at CPU start
 *   IRQ mask       MSR DAIFSET/DAIFCLR, #2    pthread_sigmask(irq_set)
 *   IRQ state      MRS DAIF                   sigismember(old, tick)
 *   atomics        LDAXR/STLXR + DMB ISH      __atomic_* , seq_cst
 *   in-handler     _in_isr[]                  _in_isr[]  (identical)
 */

#ifndef CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_
#define CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_

#if defined(OS_USE_OS_APP_CONFIG_H)
#include <cmsis-plus/os-app-config.h>
#endif

#include <cmsis-plus/rtos/os-c-decls.h>

#ifdef __cplusplus

#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#include <cmsis-plus/diag/trace.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wc++98-compat"
#endif

namespace os
{
  namespace rtos
  {
    namespace port
    {
      /*
       * Which CPU is running this code.
       *
       * This is the one deliberate use of native thread-local storage in the
       * whole port, and it is the one use that is correct by construction: a
       * host thread IS a CPU, so storage private to a host thread is storage
       * private to a CPU. It answers "where am I", never "what was I doing".
       *
       * Everything else must obey the opposite rule. A µOS++ thread migrates
       * between CPUs, so `errno` and any `thread_local` it touches belong to
       * whichever host thread happens to be running it at that instant. No
       * port or application state may live there across a switch point --
       * read errno only inside the critical section that made the call.
       */
      extern thread_local unsigned _this_cpu;

      namespace scheduler
      {
        inline unsigned __attribute__ ((always_inline))
        port_cpu_id_inline (void)
        {
          return _this_cpu;
        }

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
          return lock_state[port_cpu_id_inline ()] != state::unlocked;
        }

        /* SMP: acquire the scheduler lock word.
         *
         * A spin, exactly as on ARM, and for the same reason: this lock is
         * held for a handful of instructions and is taken from inside signal
         * handlers, where a pthread_mutex would be a function call into code
         * that is not async-signal-safe. The pause hint keeps a spinning CPU
         * from starving the one that holds it when the host oversubscribes. */
        inline void __attribute__ ((always_inline))
        _smp_klock_raw_acquire (void)
        {
          while (__atomic_exchange_n (&_smp_klock.lock, 1u, __ATOMIC_ACQUIRE)
                 != 0u)
            {
#if defined(__x86_64__) || defined(__i386__)
              __builtin_ia32_pause ();
#elif defined(__aarch64__) || defined(__arm__)
              __asm__ volatile("yield" ::: "memory");
#endif
            }
        }

        inline void __attribute__ ((always_inline))
        _smp_klock_raw_release (void)
        {
          __atomic_store_n (&_smp_klock.lock, 0u, __ATOMIC_RELEASE);
        }

        inline unsigned __attribute__ ((always_inline))
        port_smp_depth (void)
        {
          return _smp_klock.depth;
        }

        void
        wait_for_interrupt (void);

      } /* namespace scheduler */

      namespace interrupts
      {
        inline bool __attribute__ ((always_inline))
        in_handler_mode (void)
        {
          const unsigned cpu = scheduler::port_cpu_id_inline ();
          return ((cpu < OS_NCPU) && _in_isr[cpu]) || (signal_nesting != 0);
        }

        inline bool __attribute__ ((always_inline))
        is_priority_valid (void)
        {
          return true;
        }

        /*
         * Enter an IRQ critical section: mask this CPU's interrupts and take
         * the kernel lock. Same two steps, same order, as AArch64's.
         *
         * pthread_sigmask, not sigprocmask. In a multithreaded process the
         * behaviour of sigprocmask is unspecified (POSIX.1-2017, and glibc
         * documents it as such), and upstream's port used it throughout
         * because upstream had exactly one thread. The replacement is not a
         * workaround but the better abstraction: a per-thread signal mask is
         * per-CPU interrupt masking, which is what a critical section has
         * always meant.
         */
        inline rtos::interrupts::state_t __attribute__ ((always_inline))
        critical_section::enter (void)
        {
          sigset_t old;
          ::pthread_sigmask (SIG_BLOCK, &irq_set, &old);

          const unsigned cpu = scheduler::port_cpu_id_inline ();
          if (scheduler::_smp_klock.owner != cpu)
            {
              scheduler::_smp_klock_raw_acquire ();
              scheduler::_smp_klock.owner = cpu;
            }
          scheduler::_smp_klock.depth = scheduler::_smp_klock.depth + 1;

          return ::sigismember (&old, clock::signal_number ()) != 0;
        }

        inline void __attribute__ ((always_inline))
        critical_section::exit (rtos::interrupts::state_t state)
        {
          const unsigned cpu = scheduler::port_cpu_id_inline ();
          if (scheduler::_smp_klock.owner == cpu
              && scheduler::_smp_klock.depth > 0)
            {
              const uint32_t d = scheduler::_smp_klock.depth - 1;
              scheduler::_smp_klock.depth = d;
              if (d == 0)
                {
                  scheduler::_smp_klock.owner = SMP_NO_OWNER;
                  scheduler::_smp_klock_raw_release ();
                }
            }

          ::pthread_sigmask (state ? SIG_BLOCK : SIG_UNBLOCK, &irq_set,
                             nullptr);
        }

        inline rtos::interrupts::state_t __attribute__ ((always_inline))
        uncritical_section::enter (void)
        {
          sigset_t old;
          ::pthread_sigmask (SIG_UNBLOCK, &irq_set, &old);
          return ::sigismember (&old, clock::signal_number ()) != 0;
        }

        inline void __attribute__ ((always_inline))
        uncritical_section::exit (rtos::interrupts::state_t state)
        {
          ::pthread_sigmask (state ? SIG_BLOCK : SIG_UNBLOCK, &irq_set,
                             nullptr);
        }

      } /* namespace interrupts */

      namespace this_thread
      {
        inline void __attribute__ ((always_inline))
        prepare_suspend (void)
        {
        }
      } /* namespace this_thread */

      inline constexpr bool __attribute__ ((always_inline))
      clock_highres::has_hardware_counter (void) noexcept
      {
        return true;
      }

      inline uint64_t __attribute__ ((always_inline))
      clock_highres::hardware_counter (void) noexcept
      {
        struct timespec tp;
        ::clock_gettime (CLOCK_MONOTONIC, &tp);
        return static_cast<uint64_t> (tp.tv_sec) * 1000000ULL
               + static_cast<uint64_t> (tp.tv_nsec) / 1000ULL;
      }

    } /* namespace port */
  } /* namespace rtos */
} /* namespace os */

#pragma GCC diagnostic pop

#endif /* __cplusplus */

#endif /* CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_ */
