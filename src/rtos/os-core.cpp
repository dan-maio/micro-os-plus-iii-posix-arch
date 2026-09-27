/*
 * os-core.cpp - the port's half of the µOS++ III SMP scheduler, POSIX host.
 *
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) 2016-2025 Liviu Ionescu. All rights reserved.
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 *
 * ---------------------------------------------------------------------------
 * The model: a host thread IS a CPU.
 *
 * OS_NCPU host threads are created at startup and never destroyed. Each one
 * runs the scheduler and is, for every purpose the kernel can observe, a
 * core: it has its own interrupt mask (its signal mask), its own tick
 * (its own timer_create timer), its own handler-mode flag and its own entry
 * in lock_state[] and _port_ctx_pending[].
 *
 * µOS++ threads remain ucontext contexts switched WITHIN a CPU, which is
 * upstream's machinery, preserved deliberately. What is new is that a context
 * saved by one CPU may be resumed by another -- and that is the only genuinely
 * hard part of this port, because it is a data race unless the handover is
 * ordered. See switch_stacks() below.
 *
 * Read this file beside micro-os-plus-iii-aarch64/src/rtos/os-core.cpp. The
 * two have the same functions doing the same things in the same order.
 * ---------------------------------------------------------------------------
 */

#if defined(__APPLE__) || defined(__linux__)

#include <cassert>
#include <cstdlib>
#include <cstring>

#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/rtos/os-hooks.h>
#include <cmsis-plus/rtos/port/os-inlines.h>

#include <host_cpu.hpp>

#include <sys/utsname.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wc++98-compat"
#endif

extern "C" unsigned
port_cpu_id (void);

extern "C" void
os_systick_handler (void);

extern os::rtos::thread* os_idle_thread;

#if defined(OS_USE_SMP_SCHEDULER)
namespace os
{
  namespace rtos
  {
    namespace scheduler
    {
      extern thread* os_idle_thread_core[OS_NCPU];
    }
  }
}
#endif /* defined(OS_USE_SMP_SCHEDULER) */

namespace os
{
  namespace rtos
  {
    namespace port
    {
      // ----------------------------------------------------------------------

      thread_local unsigned _this_cpu = 0;


      namespace interrupts
      {
        sigset_t irq_set;

        extern "C" volatile bool _in_isr[OS_NCPU];
        volatile bool _in_isr[OS_NCPU] = {};

        extern "C" volatile uint32_t signal_nesting;
        volatile uint32_t signal_nesting = 0;
      } /* namespace interrupts */

      namespace scheduler
      {
        volatile state_t lock_state[OS_NCPU] = {};

        smp_klock_t _smp_klock = { 0, SMP_NO_OWNER, 0 };
        volatile unsigned _port_ctx_pending[OS_NCPU] = {};

        // --------------------------------------------------------------------

        void
        greeting (void)
        {
          /* struct */ utsname name;
          if (::uname (&name) != -1)
            {
              trace::printf ("POSIX synthetic SMP, running on %s %s %s",
                             name.machine, name.sysname, name.release);
            }
          else
            {
              trace::printf ("POSIX synthetic SMP");
            }

          trace::printf ("; %d CPU%s, %d Hz tick, preemptive\n", OS_NCPU,
                         (OS_NCPU == 1) ? "" : "s",
                         OS_INTEGER_SYSTICK_FREQUENCY_HZ);
        }

        result_t
        initialize (void)
        {
          // Must be done before the first critical section: every mask
          // operation in this port names this set.
          ::sigemptyset (&interrupts::irq_set);
          ::sigaddset (&interrupts::irq_set, clock::signal_number ());
          ::sigaddset (&interrupts::irq_set, clock::ipi_signal_number ());

          for (unsigned c = 0; c < OS_NCPU; ++c)
            {
              lock_state[c] = state::init;
              _port_ctx_pending[c] = 0;
              interrupts::_in_isr[c] = false;
            }

          host_cpu::install_handlers ();

          /*
           * The application's hardware hooks.
           *
           * On a bare-metal target the kernel's own src/startup/startup.cpp
           * calls these two before main(), and every carried test relies on
           * that: os_startup_initialize_hardware() is where a test brings up
           * its console, prints its banner, installs the free store and calls
           * exception::init().
           *
           * Here there is no such startup. Upstream's NOTES.md states the
           * rule -- "For portability reasons, execution starts in the main()
           * function" -- and startup.cpp is __ARM_EABI__-guarded from end to
           * end, so nothing calls them at all. The first thing every test's
           * main() calls is scheduler::initialize(), so this is where that
           * startup belongs, and the tests need no edit.
           *
           * Found by running: without this the banner never printed, the
           * application free store was never installed (the kernel's
           * malloc resource stayed in place), and exception::init() never
           * ran -- so the first SMP fault reported nothing at all.
           *
           * After install_handlers(), deliberately: exception::init() wants
           * the alternate signal stack that call sets up.
           */
          os_startup_initialize_hardware_early ();
          os_startup_initialize_hardware ();

          // Interrupts masked until start(), as every port does.
          ::pthread_sigmask (SIG_BLOCK, &interrupts::irq_set, nullptr);

          return result::ok;
        }

        // --------------------------------------------------------------------

        state_t
        locked (state_t state)
        {
          os_assert_throw (!interrupts::in_handler_mode (), EPERM);

          if (state == state::locked)
            {
              // Block the tick BEFORE reading the CPU id. The tick handler
              // swapcontext()s, and the thread can resume on another host
              // thread: read before the block, the id could name the CPU it
              // left, and lock_state[] and the kernel lock would be taken for
              // that CPU, never to be released by the matching unlock.
              ::pthread_sigmask (SIG_BLOCK, &interrupts::irq_set, nullptr);
              const unsigned cpu = port_cpu_id ();
              state_t tmp = lock_state[cpu];
              if (tmp != state::locked)
                {
                  if (_smp_klock.owner != cpu)
                    {
                      _smp_klock_raw_acquire ();
                      _smp_klock.owner = cpu;
                    }
                  _smp_klock.depth = _smp_klock.depth + 1;
                  lock_state[cpu] = state::locked;
                }
              return tmp;
            }
          else
            {
              // Holding the lock, the tick is blocked and the thread cannot
              // move; not holding it, both CPUs read "unlocked" anyway.
              const unsigned cpu = port_cpu_id ();
              state_t tmp = lock_state[cpu];
              if (tmp != state::unlocked)
                {
                  lock_state[cpu] = state::unlocked;
                  if (_smp_klock.owner == cpu && _smp_klock.depth > 0)
                    {
                      const uint32_t d = _smp_klock.depth - 1;
                      _smp_klock.depth = d;
                      if (d == 0)
                        {
                          _smp_klock.owner = SMP_NO_OWNER;
                          _smp_klock_raw_release ();
                        }
                    }
                  ::pthread_sigmask (SIG_UNBLOCK, &interrupts::irq_set,
                                     nullptr);
                }
              return tmp;
            }
        }

        // --------------------------------------------------------------------

        void
        wait_for_interrupt (void)
        {
          // The idle thread's "WFI". sigsuspend() unblocks this CPU's tick
          // and IPI and sleeps until one of them is delivered AND handled, so
          // an idle CPU costs nothing while still being preemptible.
          sigset_t mask;
          ::pthread_sigmask (SIG_SETMASK, nullptr, &mask);
          ::sigdelset (&mask, clock::signal_number ());
          ::sigdelset (&mask, clock::ipi_signal_number ());
          ::sigsuspend (&mask);
        }

        // --------------------------------------------------------------------

        /*
         * The context switch.
         *
         * Named switch_stacks() because that is what the kernel calls this
         * seam on every port, and because it is a friend of rtos::thread --
         * which is how it may touch context_ at all. Unlike the ARM ports it
         * performs the switch itself rather than returning a stack pointer to
         * an assembly restore path; there is no assembly here to return to.
         *
         * THE DEFERRED PUBLISH.
         *
         * The SMP picker in the kernel skips any thread whose stack_ptr is
         * null, meaning "not safe to claim". The window that rule exists for
         * is real here too: between the moment this CPU decides to leave
         * old_thread and the moment swapcontext() has finished writing
         * old_thread's registers into its ucontext, another CPU must not
         * resume it -- it would run a half-saved context.
         *
         * So the publish cannot be done by the CPU that is leaving: once
         * swapcontext() returns, this CPU is already executing the INCOMING
         * thread. It is done by whoever arrives next on this CPU instead. The
         * outgoing thread's address and value are left in this CPU's slot,
         * and the first thing any resumed context does -- here, after
         * swapcontext(), and in host_cpu's trampoline for a context that has
         * never run -- is publish it.
         *
         * This is the same mechanism as AArch64's _smp_pub_addr/_smp_pub_val
         * pair, which its assembly restore path applies only after SP has
         * left the outgoing stack. Same hazard, same answer, different
         * machine.
         */
        stack::element_t*
        switch_stacks (stack::element_t* sp)
        {
          (void)sp;

          /*
           * MASK THIS CPU FIRST. The switch must be atomic with respect to
           * this CPU's own interrupts, and on the ARM ports it is atomic for
           * free: the whole of switch_stacks() runs inside the IRQ path, with
           * interrupts already masked by the exception entry.
           *
           * Here it is not free. reschedule() reaches this function directly
           * from thread mode, where this CPU's tick is unmasked -- so the
           * timer could land between clearing old_thread's publish flag and
           * swapcontext() finishing the save, and the handler would re-enter
           * this same function on a half-performed switch. That is a
           * genuinely reentrant context switch, and it was the cause of the
           * intermittent SIGSEGV this port showed on smp_test2 (roughly three
           * runs in five) the first time it ran multi-core.
           *
           * The mask is saved by swapcontext() into the outgoing context and
           * restored from the incoming one, so it follows the thread across
           * CPUs, which is exactly what interrupt state should do.
           */
          sigset_t saved_mask;
          ::pthread_sigmask (SIG_BLOCK, &interrupts::irq_set, &saved_mask);

          const unsigned cpu = port_cpu_id ();

          // Take the kernel lock outright. reschedule() has already returned
          // early if this CPU still owns it, so it is not held here.
          _smp_klock_raw_acquire ();
          _smp_klock.owner = cpu;
          _smp_klock.depth = 1;

          rtos::thread* old_thread = host_cpu::current_thread (cpu);

          stack::element_t** pub_addr
              = &old_thread->context_.port_.stack_ptr;
          // Any stable non-null value: to the kernel this field is a flag,
          // and the only thing it ever asks is whether it is null.
          stack::element_t* pub_val = reinterpret_cast<stack::element_t*> (
              &old_thread->context_.port_.ucontext);

          // Park the outgoing thread before the picker can see it. It may
          // still re-pick it -- the kernel's test is
          // `th == old_thread || stack_ptr != nullptr` -- which is correct,
          // because this CPU never left it.
          __atomic_store_n (pub_addr, static_cast<stack::element_t*> (nullptr),
                            __ATOMIC_RELEASE);

          rtos::scheduler::internal_switch_threads ();

          rtos::thread* new_thread = host_cpu::current_thread (cpu);

          if (new_thread == nullptr)
            {
              trace::printf (
                  "\n!!! no ready thread and no idle thread on CPU %u !!!\n",
                  cpu);
              ::abort ();
            }

          if (new_thread == old_thread)
            {
              // Nothing to do; republish immediately and let go.
              __atomic_store_n (pub_addr, pub_val, __ATOMIC_RELEASE);
              _smp_klock.depth = 0;
              _smp_klock.owner = SMP_NO_OWNER;
              _smp_klock_raw_release ();
              ::pthread_sigmask (SIG_SETMASK, &saved_mask, nullptr);
              return nullptr;
            }

          os_impl_ucontext_t* new_uc = &new_thread->context_.port_.ucontext;

          // Claim the incoming context: from here no other CPU may take it.
          __atomic_store_n (&new_thread->context_.port_.stack_ptr,
                            static_cast<stack::element_t*> (nullptr),
                            __ATOMIC_RELEASE);

          host_cpu::defer_publish (cpu, pub_addr, pub_val);

          /* Release the kernel lock. THE ORDER MATTERS, and it is the same
           * order and the same reason as the AArch64 port documents at
           * length: owner and depth are cleared BEFORE the lock word.
           *
           * Storing the lock word first opens a window in which another CPU
           * wins the lock and installs its own owner/depth, which the two
           * stores below then wipe. Its critical_section::exit() is guarded
           * by (owner == cpu && depth > 0), so it never clears the lock word
           * again, and every CPU spins for ever -- a silent, total freeze.
           * On the ARM ports that was the root cause of the smp_test4
           * hardware deadlock. */
          _smp_klock.depth = 0;
          _smp_klock.owner = SMP_NO_OWNER;
          _smp_klock_raw_release ();

          os_impl_ucontext_t* old_uc = &old_thread->context_.port_.ucontext;

          /* Tell AddressSanitizer the stack is about to move, and where to.
           * `asan_save` lives on the OUTGOING thread's stack, so it is still
           * there -- and still this thread's -- whenever and on whichever CPU
           * that thread is resumed. Compiles to nothing without -fsanitize=
           * address; see host_cpu.hpp. */
          void* asan_save = nullptr;
          host_cpu::asan_start_switch (&asan_save,
                                       new_thread->stack ().bottom (),
                                       new_thread->stack ().size ());

          if (os_impl_swapcontext (old_uc, new_uc) != 0)
            {
              trace::printf ("port::scheduler::%s() swapcontext failed: %s\n",
                             __func__, strerror (errno));
              ::abort ();
            }

          // Resumed -- and NOT necessarily on the CPU that left. Everything
          // below must re-read the CPU index; nothing captured above is
          // valid any more.
          host_cpu::asan_finish_switch (asan_save);
          host_cpu::publish_pending ();

          // Unmask last, and only now: everything above this line is the
          // switch, and the switch is not interruptible.
          ::pthread_sigmask (SIG_SETMASK, &saved_mask, nullptr);

          return nullptr;
        }

        // --------------------------------------------------------------------

        void
        reschedule (void)
        {
          const unsigned cpu = port_cpu_id ();

          if (rtos::scheduler::locked ()
              || (rtos::interrupts::in_handler_mode ()
                  && !rtos::scheduler::preemptive ()))
            {
              return;
            }

          // If this CPU still owns the kernel lock -- resume_one() called
          // from inside an interrupts::critical_section, as message_queue's
          // send/receive do -- switching here would strand owner and depth on
          // the outgoing thread's context and deadlock the next acquirer.
          // Defer to the next tick, which runs once that section has exited.
          if (_smp_klock.owner == cpu && _smp_klock.depth > 0)
            {
              _port_ctx_pending[cpu] = 1;
              return;
            }

          if (rtos::interrupts::in_handler_mode ())
            {
              // Taken on the way out of the handler, where the mask is right.
              _port_ctx_pending[cpu] = 1;
              return;
            }

          switch_stacks (nullptr);
        }

        // --------------------------------------------------------------------

        [[noreturn]] void
        start (void)
        {
          ::pthread_sigmask (SIG_BLOCK, &interrupts::irq_set, nullptr);

          // A context to switch AWAY from. The very first switch has to save
          // something, and this CPU's host-thread context is not a µOS++
          // thread. The ARM ports use a zeroed fake_thread for the same
          // reason; this one is never scheduled again, so it needs no stack.
          static os_thread_t fake_thread[OS_NCPU];
          const unsigned cpu = port_cpu_id ();
          memset (&fake_thread[cpu], 0, sizeof (os_thread_t));
          fake_thread[cpu].name = "fake_thread";
          host_cpu::current_thread (cpu)
              = reinterpret_cast<rtos::thread*> (&fake_thread[cpu]);

          for (unsigned c = 0; c < OS_NCPU; ++c)
            {
              lock_state[c] = state::init;
            }

#if defined(OS_USE_SMP_SCHEDULER)
          // The SMP picker asks each CPU for its own idle thread; CPU 0's is
          // the kernel's. The secondaries' are installed by the board, from
          // test-smp-boot.cpp, exactly as on the ARM boards.
          rtos::scheduler::os_idle_thread_core[0] = ::os_idle_thread;
#endif /* defined(OS_USE_SMP_SCHEDULER) */

          host_cpu::start_this_cpu_tick ();

          reschedule ();

          // Reached only if reschedule() found nothing to run at all.
          ::pthread_sigmask (SIG_UNBLOCK, &interrupts::irq_set, nullptr);
          for (;;)
            {
              ::pause ();
            }
        }

      } /* namespace scheduler */

      // ----------------------------------------------------------------------

      void
      context::create (void* context, void* func, void* args)
      {
        /* class */ rtos::thread::context* th_ctx
            = static_cast</* class */ rtos::thread::context*> (context);

        memset (&th_ctx->port_, 0, sizeof (th_ctx->port_));

        os_impl_ucontext_t* ctx = &th_ctx->port_.ucontext;

        if (os_impl_getcontext (ctx) != 0)
          {
            trace::printf ("port::context::%s() getcontext failed: %s\n",
                           __func__, strerror (errno));
            ::abort ();
          }

        // The context itself is not wanted; makecontext() merely requires one
        // obtained from getcontext().
        ctx->uc_link = nullptr;
        ctx->uc_stack.ss_sp = th_ctx->stack ().bottom ();
        ctx->uc_stack.ss_size = th_ctx->stack ().size ();
        ctx->uc_stack.ss_flags = 0;

        /*
         * The starting signal mask: THIS CPU'S INTERRUPTS MASKED.
         *
         * Two wrong answers were tried before this one, which is why the
         * reasoning is written down.
         *
         * getcontext() snapshots the CALLER's mask, and a thread is very
         * often created from inside a critical section -- so inheriting it
         * would start the thread with interrupts masked FOR EVER, because
         * nothing would ever unmask them. Clearing the mask entirely is
         * wrong in the opposite direction: a context resumed by
         * switch_stacks() comes back with interrupts masked and unmasks only
         * after it has discharged this CPU's deferred publish, and a context
         * that has never run arrives on a CPU owing exactly the same publish.
         * Starting it unmasked lets a tick land inside the trampoline before
         * that publish happens -- and that tick's own switch overwrites the
         * pending slot, so the thread it was owed to is never republished and
         * is lost from the ready list for good.
         *
         * So a new context begins the way a resumed one does: masked. The
         * trampoline publishes, then unmasks, and from that point the thread
         * is preemptible like any other.
         */
        ::sigemptyset (&ctx->uc_sigmask);
        ::sigaddset (&ctx->uc_sigmask, clock::signal_number ());
        ::sigaddset (&ctx->uc_sigmask, clock::ipi_signal_number ());

        host_cpu::make_entry (ctx, func, args);

        // Published: this context has never run, so it is complete by
        // definition and any CPU may claim it.
        th_ctx->port_.stack_ptr
            = reinterpret_cast<stack::element_t*> (&th_ctx->port_.ucontext);
      }

      // ======================================================================

      void
      clock_systick::start (void)
      {
        // Each CPU arms its own timer when it starts; this is CPU 0's, and
        // the secondaries' are armed by host_cpu::start_secondary_cpus().
        // Only CPU 0 advances the kernel clock -- exactly as on the BCM2837,
        // where all four cores take a 1 ms PPI but only core 0 calls
        // os_systick_handler(). The others use theirs to preempt themselves.
      }

      // ======================================================================

      static uint64_t previous_timestamp;

      static uint64_t
      get_current_micros (void)
      {
        /* struct */ timespec tp;
        ::clock_gettime (CLOCK_MONOTONIC, &tp);

        return static_cast<uint64_t> (tp.tv_sec) * 1000000ULL
               + static_cast<uint64_t> (tp.tv_nsec) / 1000ULL;
      }

      void
      clock_highres::start (void)
      {
        previous_timestamp = get_current_micros ();
      }

      uint32_t
      clock_highres::input_clock_frequency_hz (void)
      {
        // CLOCK_MONOTONIC is read here at microsecond resolution, so a
        // higher frequency would be a claim the source cannot support.
        return 1000000;
      }

      uint32_t
      clock_highres::cycles_per_tick (void)
      {
        uint64_t ts = get_current_micros ();
        uint32_t delta = static_cast<uint32_t> (ts - previous_timestamp);

        previous_timestamp = ts;

        return delta;
      }

      uint32_t
      clock_highres::cycles_since_tick (void)
      {
        uint64_t ts = get_current_micros ();

        return static_cast<uint32_t> (ts - previous_timestamp);
      }

    } /* namespace port */
  } /* namespace rtos */
} /* namespace os */

// ----------------------------------------------------------------------------

extern "C" unsigned
port_cpu_id (void)
{
  return os::rtos::port::_this_cpu;
}

#endif /* defined(__APPLE__) || defined(__linux__) */
