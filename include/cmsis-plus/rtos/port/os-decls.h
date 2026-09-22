/*
 * os-decls.h - the C++ half of the POSIX port contract.
 *
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) 2016-2025 Liviu Ionescu. All rights reserved.
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 *
 * This port does NOT link micro-os-plus::port-smp-decls, and that target's
 * own README says why: the shared copy is written for ports whose state types
 * are integers and whose thread context is a bare stack pointer. Neither is
 * true here -- the interrupt state is a signal-mask bit, the scheduler state
 * is a bool, and the context carries a ucontext. The SMP declarations the
 * kernel actually reads (lock_state[], _smp_klock, _smp_tlock,
 * _port_ctx_pending[]) are reproduced below with the same names, the same
 * types and the same volatility, so the kernel cannot tell the two apart.
 */

#ifndef CMSIS_PLUS_RTOS_PORT_OS_DECLS_H_
#define CMSIS_PLUS_RTOS_PORT_OS_DECLS_H_

#if defined(OS_USE_OS_APP_CONFIG_H)
#include <cmsis-plus/os-app-config.h>
#endif

#include <cmsis-plus/rtos/port/os-c-decls.h>

#if !defined(OS_NCPU)
#define OS_NCPU (1)
#endif

#if !defined(OS_INTEGER_SYSTICK_FREQUENCY_HZ)
#define OS_INTEGER_SYSTICK_FREQUENCY_HZ (1000)
#endif

/*
 * Host stacks are large because a signal frame lands on them.
 *
 * Preemption here is a signal delivered on the running thread's own stack --
 * deliberately, with no SA_ONSTACK, so that the whole signal frame migrates
 * with the thread when another CPU resumes it (see host_cpu.cpp). That frame
 * is several kilobytes of ucontext plus the FPU state, so upstream's 32 KiB
 * minimum is kept rather than the 2 KiB the ARM ports use.
 */
#if !defined(OS_INTEGER_RTOS_MIN_STACK_SIZE_BYTES)
#define OS_INTEGER_RTOS_MIN_STACK_SIZE_BYTES (32 * 1024)
#endif

#if !defined(OS_INTEGER_RTOS_DEFAULT_STACK_SIZE_BYTES)
#define OS_INTEGER_RTOS_DEFAULT_STACK_SIZE_BYTES \
  (2 * OS_INTEGER_RTOS_MIN_STACK_SIZE_BYTES)
#endif

#if !defined(OS_INTEGER_RTOS_MAIN_STACK_SIZE_BYTES)
#define OS_INTEGER_RTOS_MAIN_STACK_SIZE_BYTES \
  (OS_INTEGER_RTOS_DEFAULT_STACK_SIZE_BYTES)
#endif

#if !defined(OS_INTEGER_RTOS_IDLE_STACK_SIZE_BYTES)
#define OS_INTEGER_RTOS_IDLE_STACK_SIZE_BYTES \
  (OS_INTEGER_RTOS_DEFAULT_STACK_SIZE_BYTES)
#endif

#if !defined(OS_INTEGER_RTOS_STACK_FILL_MAGIC)
#define OS_INTEGER_RTOS_STACK_FILL_MAGIC (0xEFBEADDEEFBEADDEULL)
#endif

/* Kept identical to the ARM ports so the same code reads on both. */
#if !defined(SMP_NO_OWNER)
#define SMP_NO_OWNER (0xFFFFFFFFu)
#endif

#ifdef __cplusplus

#include <signal.h>

#include <cstdint>
#include <cstddef>

#pragma GCC diagnostic push

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wc++98-compat"
#endif

namespace os
{
  namespace rtos
  {
    namespace port
    {

      namespace stack
      {
        using element_t = os_port_thread_stack_element_t;

        using allocation_element_t = os_port_thread_stack_allocation_element_t;

        constexpr std::size_t min_size_bytes
            = OS_INTEGER_RTOS_MIN_STACK_SIZE_BYTES;

        constexpr std::size_t default_size_bytes
            = OS_INTEGER_RTOS_DEFAULT_STACK_SIZE_BYTES;

        constexpr element_t magic = OS_INTEGER_RTOS_STACK_FILL_MAGIC;
      } /* namespace stack */

      namespace interrupts
      {
        using state_t = os_port_irq_state_t;

        namespace state
        {
          constexpr state_t init = false;
        } /* namespace state */

        /* The signals this port treats as interrupts: the per-CPU tick and
         * the IPI. Blocking them IS masking interrupts on this CPU. */
        extern sigset_t irq_set;

        /* Per-CPU handler-mode flag. AArch64 has the same array for the same
         * reason: there are no banked modes to ask. */
        extern "C" volatile bool _in_isr[OS_NCPU];
      } /* namespace interrupts */

      namespace scheduler
      {
        using state_t = os_port_scheduler_state_t;

        namespace state
        {
          constexpr state_t locked = true;
          constexpr state_t unlocked = false;
          constexpr state_t init = unlocked;
        } /* namespace state */

        extern volatile state_t lock_state[OS_NCPU];

        /* Byte-for-byte the ARM ports' structures: the recursive kernel lock
         * (lock word, owner CPU, nesting depth) and the timer leaf lock. */
        struct smp_klock_t
        {
          volatile uint32_t lock;
          volatile uint32_t owner;
          volatile uint32_t depth;
        };

        struct smp_tlock_t
        {
          volatile uint32_t lock;
        };

        extern smp_klock_t _smp_klock;
        extern smp_tlock_t _smp_tlock;
        extern volatile unsigned _port_ctx_pending[OS_NCPU];
      } /* namespace scheduler */

      namespace clock
      {
        /* Not SIGALRM. ITIMER_REAL and SIGALRM are process-wide; this port
         * gives every CPU its own timer_create() timer, and a real-time
         * signal because those queue rather than coalesce.
         *
         * Functions, not constants: glibc's SIGRTMIN expands to a call to
         * __libc_current_sigrtmin(), so it is not a constant expression. */
        inline int
        signal_number (void)
        {
          return SIGRTMIN;
        }

        inline int
        ipi_signal_number (void)
        {
          return SIGRTMIN + 1;
        }
      } /* namespace clock */

      using thread_context_t = os_port_thread_context_t;

    } /* namespace port */
  } /* namespace rtos */
} /* namespace os */

#pragma GCC diagnostic pop

#endif /* __cplusplus */

#endif /* CMSIS_PLUS_RTOS_PORT_OS_DECLS_H_ */
