/*
 * host_cpu.hpp - the CPU model of the POSIX port: a host thread IS a CPU.
 *
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 *
 * On the ARM ports the equivalent of this header is split between the silicon
 * (the generic timer, the GIC/mailbox IPI, MPIDR) and the board (the spin
 * table that releases the secondary cores). Here the silicon is the host
 * kernel, so it is all one file, and it is port code rather than board code.
 */

#ifndef MICRO_OS_PLUS_III_POSIX_ARCH_HOST_CPU_HPP_
#define MICRO_OS_PLUS_III_POSIX_ARCH_HOST_CPU_HPP_

#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/rtos/port/os-decls.h>

namespace host_cpu
{
  using element_t = os::rtos::port::stack::element_t;

  /*
   * "Which thread is running on this CPU."
   *
   * The kernel spells this two different ways: under the SMP scheduler
   * `scheduler::current_thread_` is an array indexed by CPU, and without it
   * a single pointer (os-sched.h, guarded on OS_USE_SMP_SCHEDULER, which
   * uos_add_app() derives from NCPU > 1).
   *
   * One accessor keeps the difference in one place, so the rest of the port
   * is written once and serves both. This is the same dual-branch arrangement
   * the cortexm port uses to run its three STM32 boards at OS_NCPU=1 off the
   * RP2350's SMP core.
   */
  inline os::rtos::thread* volatile&
  current_thread (unsigned cpu)
  {
#if defined(OS_USE_SMP_SCHEDULER)
    return os::rtos::scheduler::current_thread_[cpu];
#else
    (void)cpu;
    return os::rtos::scheduler::current_thread_;
#endif
  }

  /* Install the tick and IPI signal handlers, process-wide. Called once,
   * from port::scheduler::initialize(). */
  void
  install_handlers (void);

  /* Give the calling CPU an alternate stack for fault reporting. Per host
   * thread, because sigaltstack() is, and that is correct here: reporting a
   * fault does not migrate, so it may use storage the CPU owns. */
  void
  install_fault_stack (void);

  /* Arm the calling CPU's own periodic timer, at
   * OS_INTEGER_SYSTICK_FREQUENCY_HZ. Every CPU has one; only CPU 0 advances
   * the kernel clock with it. */
  void
  start_this_cpu_tick (void);

  /* Create the OS_NCPU-1 secondary host threads. Each sets its own CPU index,
   * arms its own tick and enters the scheduler, which is what a secondary
   * core does after the spin table releases it. */
  void
  start_secondary_cpus (void);

  /* Ask another CPU to re-pick. pthread_kill() of the IPI signal, which is
   * this port's SGI / mailbox doorbell. */
  void
  send_ipi (unsigned cpu);

  /* Build a never-run context's entry point. Wraps the kernel's thread body
   * in the trampoline that discharges this CPU's deferred publish before the
   * body runs -- a context that has never run still arrives on a CPU that
   * has just left another thread behind. */
  void
  make_entry (os_impl_ucontext_t* ctx, void* func, void* args);

  /* Leave `*addr = val` for whoever runs next on this CPU. See the long
   * comment on port::scheduler::switch_stacks(). */
  void
  defer_publish (unsigned cpu, element_t** addr, element_t* val);

  /* Discharge this CPU's deferred publish, if it has one. Called on every
   * arrival into a context: after swapcontext(), and in the trampoline. */
  void
  publish_pending (void);

} /* namespace host_cpu */

#endif /* MICRO_OS_PLUS_III_POSIX_ARCH_HOST_CPU_HPP_ */
