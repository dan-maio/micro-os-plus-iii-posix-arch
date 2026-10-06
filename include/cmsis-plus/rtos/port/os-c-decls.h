/*
 * os-c-decls.h - the width-dependent half of the POSIX port contract.
 *
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) 2016-2025 Liviu Ionescu. All rights reserved.
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 *
 * Derived from upstream micro-os-plus-iii-posix-arch v1.0.1. What changed and
 * why is in os-decls.h, beside the declarations it changed.
 */

#ifndef CMSIS_PLUS_RTOS_PORT_OS_C_DECLS_H_
#define CMSIS_PLUS_RTOS_PORT_OS_C_DECLS_H_

#include <stdint.h>

#if !defined(_XOPEN_SOURCE)
#error This port requires defining _XOPEN_SOURCE=600L or 700L globally
#endif

#if defined(OS_INCLUDE_LIBUCONTEXT)

#include <libucontext/libucontext.h>

#define os_impl_ucontext_t libucontext_ucontext_t
#define os_impl_getcontext libucontext_posix_getcontext
#define os_impl_makecontext libucontext_makecontext
#define os_impl_setcontext libucontext_posix_setcontext
#define os_impl_swapcontext libucontext_posix_swapcontext

#else

#include <ucontext.h>

#define os_impl_ucontext_t ucontext_t
#define os_impl_getcontext getcontext
#define os_impl_makecontext makecontext
#define os_impl_setcontext setcontext
#define os_impl_swapcontext swapcontext

#endif // defined(OS_INCLUDE_LIBUCONTEXT)

#include <signal.h>
#include <stdbool.h>

// Must match port::clock::timestamp_t
typedef uint64_t os_port_clock_timestamp_t;

// Must match port::clock::duration_t
typedef uint32_t os_port_clock_duration_t;

// Must match port::clock::offset_t
typedef uint64_t os_port_clock_offset_t;

typedef uint64_t os_port_thread_stack_element_t;
typedef uint64_t os_port_thread_stack_allocation_element_t;

/*
 * The thread context.
 *
 * `stack_ptr` FIRST and `ucontext` second, and the order is not cosmetic.
 *
 * Upstream's context was the ucontext alone, because upstream had one CPU and
 * therefore never had to ask whether a saved context was safe to resume. The
 * SMP scheduler does ask, in the kernel, in a line no port may edit:
 *
 *   src/rtos/os-core.cpp, internal_switch_threads():
 *     && (th == old_thread || th->context_.port_.stack_ptr != nullptr)
 *
 * On the ARM ports that field is the outgoing stack pointer, published by the
 * assembly restore path only once the CPU has left the outgoing thread's
 * stack. Here it is the same gate carrying the same meaning -- "this context
 * is fully saved, another CPU may claim it" -- and nothing more: the register
 * state lives in the ucontext. A CPU clears it when it claims a thread and
 * the NEXT context to run on that CPU publishes it, which is the whole of the
 * deferred-publish rule. See host_cpu.cpp.
 */
typedef struct
{
  os_port_thread_stack_element_t* stack_ptr;
  os_impl_ucontext_t ucontext;
} os_port_thread_context_t;

typedef bool os_port_scheduler_state_t;

/* True when this CPU's interrupt signals are blocked. A per-thread signal
 * mask IS per-CPU interrupt masking, because a host thread IS a CPU. */
typedef bool os_port_irq_state_t;

#endif /* CMSIS_PLUS_RTOS_PORT_OS_C_DECLS_H_ */
