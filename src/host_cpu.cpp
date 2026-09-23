/*
 * host_cpu.cpp - host threads as CPUs: bring-up, per-CPU tick, IPI.
 *
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 */

#if defined(__APPLE__) || defined(__linux__)

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/rtos/port/os-inlines.h>

#include <host_cpu.hpp>

extern "C" void
os_systick_handler (void);

extern "C" unsigned
port_cpu_id (void);

/* Defined by the board (test/boards/<board>/src/smp.cpp), with the same
 * meaning as on the ARM boards: 0 = not started, 3 = scheduler entered. The
 * tests wait on it before they start timing anything. A board that does not
 * define it fails to link, which is the point. */
extern "C" volatile uint32_t g_core_stage[OS_NCPU];

namespace
{
  using namespace os::rtos::port;

  /* Every CPU's host thread, so one can be signalled by another. */
  pthread_t g_cpu_thread[OS_NCPU];
  volatile bool g_cpu_up[OS_NCPU];

  /* The deferred-publish slot, one per CPU. */
  struct publish_slot
  {
    host_cpu::element_t** addr;
    host_cpu::element_t* val;
  };

  publish_slot g_publish[OS_NCPU];

  /* What a never-run context must call. Recovered by the trampoline from the
   * two pointers makecontext() carries for it. */
  using body_t = void (*) (void*);

  [[noreturn]] void
  fatal (const char* what)
  {
    std::fprintf (stderr, "\n!!! posix-arch: %s: %s\n", what,
                  std::strerror (errno));
    std::abort ();
  }

  /*
   * The interrupt path.
   *
   * Deliberately the same shape as boards/rpi-zero-2w/src/rtos/port_isr.cpp:
   * flag handler mode, do the work, mark a switch pending, clear the flag,
   * and take the switch on the way out.
   *
   * Two things are deliberate and neither is an oversight:
   *
   *  - NO SA_ONSTACK. The handler runs on the µOS++ thread's own stack, so
   *    the whole signal frame is part of the context that swapcontext() saves
   *    and therefore migrates with the thread when another CPU resumes it. An
   *    alternate stack belongs to the host thread and would be left behind.
   *    This is why OS_INTEGER_RTOS_MIN_STACK_SIZE_BYTES is 32 KiB here.
   *
   *  - The switch happens INSIDE the handler, not after sigreturn. Returning
   *    first would mean the switch only ever happened at a cooperative point,
   *    which is exactly the limitation upstream's NOTES.md records ("the
   *    scheduler runs in cooperative mode only"). swapcontext() from a
   *    handler is well defined here because the handler frame lives on the
   *    thread's stack: resuming the context resumes the handler, which then
   *    returns through sigreturn normally.
   */
  void
  irq_epilogue (unsigned cpu)
  {
    if (scheduler::_port_ctx_pending[cpu] == 0)
      {
        return;
      }

    if (os::rtos::scheduler::locked ())
      {
        // Still masked by the kernel; the next tick will find it pending.
        return;
      }

    if (scheduler::_smp_klock.owner == cpu && scheduler::_smp_klock.depth > 0)
      {
        return;
      }

    scheduler::_port_ctx_pending[cpu] = 0;
    scheduler::switch_stacks (nullptr);
  }

  /* Put `errno` back, out of line.
   *
   * Out of line on purpose. `errno` is `*__errno_location()`, which is native
   * TLS, and the caller read it BEFORE a swapcontext() that may have resumed
   * this thread on a different CPU -- that is, on a different host thread,
   * with a different errno. A compiler that cached the address across the
   * switch would write the value into the host thread the thread LEFT. This
   * port bans native TLS across a switch point for exactly that reason, and a
   * noinline call is how the ban is honoured here: the address is resolved
   * inside this function, after the switch, on whichever CPU is running now.
   */
  [[gnu::noinline]] void
  restore_errno (int value)
  {
    errno = value;
  }

  void
  tick_handler (int, siginfo_t*, void*)
  {
    /* errno belongs to the THREAD, and on this port the thread is the uOS++
     * one, not the host thread it is borrowing.
     *
     * This handler does not, in general, return to where it was raised: its
     * epilogue switches contexts, so control leaves on one thread's stack and
     * comes back -- possibly on another CPU, possibly much later -- when THIS
     * thread is resumed. Everything the handler and every thread scheduled in
     * between did to errno is therefore visible to the interrupted code
     * unless it is put back. On silicon there is no errno to spoil; here
     * there is.
     *
     * `saved` is a local, so like the ASan fibre save it rides the
     * interrupted thread's own stack and is still correct wherever that
     * thread comes back. */
    const int saved = errno;

    const unsigned cpu = port_cpu_id ();
    if (cpu >= OS_NCPU)
      {
        restore_errno (saved);
        return;
      }

    interrupts::_in_isr[cpu] = true;

    // Only CPU 0 advances the kernel clock. Every CPU tickles itself into
    // re-picking. This is the BCM2837 arrangement exactly: four cores take
    // the 1 ms PPI, one calls os_systick_handler().
    if (cpu == 0)
      {
        os_systick_handler ();
      }

    scheduler::_port_ctx_pending[cpu] = 1;

    interrupts::_in_isr[cpu] = false;

    irq_epilogue (cpu);

    // Resumed. See the comment at the top of this function.
    restore_errno (saved);
  }

  void
  ipi_handler (int, siginfo_t*, void*)
  {
    // Same reasoning as tick_handler(); this one switches contexts too.
    const int saved = errno;

    const unsigned cpu = port_cpu_id ();
    if (cpu >= OS_NCPU)
      {
        restore_errno (saved);
        return;
      }

    interrupts::_in_isr[cpu] = true;
    scheduler::_port_ctx_pending[cpu] = 1;
    interrupts::_in_isr[cpu] = false;

    irq_epilogue (cpu);

    restore_errno (saved);
  }

  /* One per CPU, never shared: a CPU reporting a fault is not going anywhere
   * else while it does so. SIGSTKSZ is a minimum, and the reporting path
   * formats numbers, so it gets room. */
  char g_fault_stack[OS_NCPU][64 * 1024];

  void
  arm_tick (void)
  {
    /* struct */ sigevent sev;
    std::memset (&sev, 0, sizeof (sev));

#if defined(__linux__)
    // SIGEV_THREAD_ID, not ITIMER_REAL.
    //
    // ITIMER_REAL delivers SIGALRM to an ARBITRARY thread of the process, so
    // with N CPUs the tick would land on whichever host thread the kernel
    // happened to choose -- a tick is not a per-CPU event any more, and the
    // preemption of CPU 2 could be charged to CPU 0. A per-thread timer is
    // what a per-core timer actually is.
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev._sigev_un._tid = static_cast<int> (::syscall (SYS_gettid));
#else
    sev.sigev_notify = SIGEV_SIGNAL;
#endif
    sev.sigev_signo = clock::signal_number ();

    timer_t timer;
    if (::timer_create (CLOCK_MONOTONIC, &sev, &timer) != 0)
      {
        fatal ("timer_create");
      }

    const long period_ns = 1000000000L / OS_INTEGER_SYSTICK_FREQUENCY_HZ;

    /* struct */ itimerspec its;
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = period_ns;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = period_ns;

    if (::timer_settime (timer, 0, &its, nullptr) != 0)
      {
        fatal ("timer_settime");
      }
  }

  void*
  secondary_cpu_body (void* arg)
  {
    const unsigned cpu
        = static_cast<unsigned> (reinterpret_cast<uintptr_t> (arg));

    _this_cpu = cpu;

    host_cpu::install_fault_stack ();

    ::pthread_sigmask (SIG_BLOCK, &interrupts::irq_set, nullptr);

    // A context to switch away from, as port::scheduler::start() builds for
    // CPU 0. A secondary core does exactly this after the spin table
    // releases it: give itself somewhere to save, then reschedule.
    static os_thread_t fake_thread[OS_NCPU];
    std::memset (&fake_thread[cpu], 0, sizeof (os_thread_t));
    fake_thread[cpu].name = "fake_thread_sec";
    host_cpu::current_thread (cpu)
        = reinterpret_cast<os::rtos::thread*> (&fake_thread[cpu]);

    g_cpu_up[cpu] = true;

    host_cpu::start_this_cpu_tick ();

    g_core_stage[cpu] = 3;

    scheduler::reschedule ();

    // Only if this CPU never found anything to run.
    ::pthread_sigmask (SIG_UNBLOCK, &interrupts::irq_set, nullptr);
    for (;;)
      {
        ::pause ();
      }

    return nullptr;
  }

  [[noreturn]] void
  trampoline (void* func, void* args)
  {
    // Arrival. The switch that brought us here started on another stack; this
    // tells ASan the move is complete and that THIS stack is the live one.
    // nullptr because a context that has never run has no outgoing fibre
    // bookkeeping of its own to restore.
    host_cpu::asan_finish_switch (nullptr);

    // A context that has never run still arrives on a CPU that has just left
    // another thread behind, so the publish is owed here too -- and it is
    // owed BEFORE this thread can be interrupted, or the tick's own switch
    // would overwrite the slot and strand the outgoing thread. context::create
    // starts every new context with this CPU's interrupts masked for exactly
    // this window; it ends here.
    host_cpu::publish_pending ();

    ::pthread_sigmask (SIG_UNBLOCK, &interrupts::irq_set, nullptr);

    reinterpret_cast<body_t> (func) (args);

    // The kernel's thread body does not return.
    std::fprintf (stderr, "\n!!! posix-arch: thread body returned\n");
    std::abort ();
  }

} /* anonymous namespace */

namespace host_cpu
{
  void
  install_handlers (void)
  {
    struct sigaction sa; // `sigaction` is also a function: the tag is required
    std::memset (&sa, 0, sizeof (sa));

    // SA_RESTART so a preempted read()/write() resumes rather than failing
    // with EINTR; the tests print through write(), a thousand times a second.
    // No SA_ONSTACK -- see irq_epilogue() above.
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    ::sigemptyset (&sa.sa_mask);
    ::sigaddset (&sa.sa_mask, clock::signal_number ());
    ::sigaddset (&sa.sa_mask, clock::ipi_signal_number ());

    sa.sa_sigaction = tick_handler;
    if (::sigaction (clock::signal_number (), &sa, nullptr) != 0)
      {
        fatal ("sigaction(tick)");
      }

    sa.sa_sigaction = ipi_handler;
    if (::sigaction (clock::ipi_signal_number (), &sa, nullptr) != 0)
      {
        fatal ("sigaction(ipi)");
      }

    g_cpu_thread[0] = ::pthread_self ();
    g_cpu_up[0] = true;
    g_core_stage[0] = 3;

    install_fault_stack ();
  }

  void
  install_fault_stack (void)
  {
    const unsigned cpu = port_cpu_id ();
    if (cpu >= OS_NCPU)
      {
        return;
      }

    stack_t ss;
    std::memset (&ss, 0, sizeof (ss));
    ss.ss_sp = g_fault_stack[cpu];
    ss.ss_size = sizeof (g_fault_stack[cpu]);
    ss.ss_flags = 0;
    ::sigaltstack (&ss, nullptr);
  }

  void
  start_this_cpu_tick (void)
  {
    arm_tick ();
  }

  void
  start_secondary_cpus (void)
  {
    for (unsigned cpu = 1; cpu < OS_NCPU; ++cpu)
      {
        pthread_attr_t attr;
        ::pthread_attr_init (&attr);

        if (::pthread_create (
                &g_cpu_thread[cpu], &attr, secondary_cpu_body,
                reinterpret_cast<void*> (static_cast<uintptr_t> (cpu)))
            != 0)
          {
            fatal ("pthread_create(cpu)");
          }

        ::pthread_attr_destroy (&attr);
      }

    // Wait for every CPU to have an index and a context to save into, the
    // way the ARM ports wait on the per-core stage flags before proceeding.
    for (unsigned cpu = 1; cpu < OS_NCPU; ++cpu)
      {
        while (!g_cpu_up[cpu])
          {
            ::usleep (100);
          }
      }
  }

  void
  send_ipi (unsigned cpu)
  {
    if (cpu < OS_NCPU && g_cpu_up[cpu])
      {
        ::pthread_kill (g_cpu_thread[cpu], clock::ipi_signal_number ());
      }
  }

  void
  make_entry (os_impl_ucontext_t* ctx, void* func, void* args)
  {
#pragma GCC diagnostic push
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wc++98-compat-pedantic"
#endif
    // Two pointer arguments. glibc carries them whole -- verified on this
    // host before it was relied on, because the interface is declared with
    // int arguments and a truncated `args` would be a pointer to nothing.
    os_impl_makecontext (
        ctx, reinterpret_cast<void (*) (void)> (trampoline), 2, func, args);
#pragma GCC diagnostic pop
  }

  void
  defer_publish (unsigned cpu, element_t** addr, element_t* val)
  {
    g_publish[cpu].addr = addr;
    g_publish[cpu].val = val;
  }

  /* See the long comment in host_cpu.hpp. Declared by hand rather than through
   * <sanitizer/asan_interface.h>, so that a toolchain without the header still
   * builds the port -- the symbols come from the ASan runtime, which is only
   * linked when -fsanitize=address is on, and the calls are compiled out
   * otherwise. */
#if defined(__SANITIZE_ADDRESS__) \
    || (defined(__has_feature) && __has_feature (address_sanitizer))
#define UOS_HAVE_ASAN 1
extern "C" void
__sanitizer_start_switch_fiber (void** fake_stack_save, const void* bottom,
                                std::size_t size);
extern "C" void
__sanitizer_finish_switch_fiber (void* fake_stack_save, const void** bottom_old,
                                 std::size_t* size_old);
#endif

  void
  asan_start_switch (void** save, const void* bottom, std::size_t size)
  {
#if defined(UOS_HAVE_ASAN)
    __sanitizer_start_switch_fiber (save, bottom, size);
#else
    (void)save;
    (void)bottom;
    (void)size;
#endif
  }

  void
  asan_finish_switch (void* save)
  {
#if defined(UOS_HAVE_ASAN)
    __sanitizer_finish_switch_fiber (save, nullptr, nullptr);
#else
    (void)save;
#endif
  }

  void
  publish_pending (void)
  {
    // The CPU index must be re-read: this runs after a swapcontext() that
    // may well have been performed by a different CPU.
    const unsigned cpu = port_cpu_id ();
    if (cpu >= OS_NCPU)
      {
        return;
      }

    element_t** addr = g_publish[cpu].addr;
    if (addr == nullptr)
      {
        return;
      }

    g_publish[cpu].addr = nullptr;
    __atomic_store_n (addr, g_publish[cpu].val, __ATOMIC_RELEASE);
  }

} /* namespace host_cpu */

extern "C" void
port_smp_ipi (unsigned cpu)
{
  host_cpu::send_ipi (cpu);
}

#endif /* defined(__APPLE__) || defined(__linux__) */
