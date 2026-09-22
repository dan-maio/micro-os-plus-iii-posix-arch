/*
 * exception_handler.cpp - synchronous faults, reported rather than silent.
 *
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 */

#if defined(__APPLE__) || defined(__linux__)

#include <csignal>
#include <cstring>
#include <initializer_list>
#include <dlfcn.h>
#include <execinfo.h>
#include <ucontext.h>
#include <unistd.h>

#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/rtos/port/os-decls.h>
#include <cmsis-plus/rtos/port/os-inlines.h>
#include <exception_handler.hpp>
#include <host_cpu.hpp>

extern "C" unsigned
port_cpu_id (void);

namespace
{
  // Everything below runs in a signal handler after the process is already
  // broken, so it uses write(2) and nothing else: no printf, no malloc, no
  // locks. A reporting path that can itself deadlock reports nothing.
  void
  emit (const char* s)
  {
    std::size_t n = 0;
    while (s[n] != '\0')
      {
        ++n;
      }
    ssize_t r = ::write (2, s, n);
    (void)r;
  }

  void
  emit_hex (std::uint64_t v)
  {
    static const char digits[] = "0123456789ABCDEF";
    char out[19] = "0x";
    int n = 2;
    for (int i = 15; i >= 0; --i)
      {
        out[n++] = digits[(v >> (i * 4)) & 0xF];
      }
    out[n] = '\0';
    emit (out);
  }

  /*
   * The program counter of the faulting instruction, from the machine context
   * the kernel handed the handler. The ARM ports report ELR for the same
   * reason: on an SMP fault the faulting address alone rarely identifies the
   * bug, and `addr2line -e <image> <pc>` names the line outright.
   */
  /*
   * The load bias of this image, captured in exception::init() -- i.e. in a
   * sane context, because dladdr() is not async-signal-safe. Reported
   * alongside the raw pc so that a position-independent executable (which is
   * the default here) can still be looked up with the static addresses in the
   * ELF file: `addr2line -e <image> <pc-bias>`.
   */
  std::uint64_t g_load_bias = 0;

  std::uint64_t
  fault_pc (void* ucontext)
  {
    if (ucontext == nullptr)
      {
        return 0;
      }
    ucontext_t* uc = static_cast<ucontext_t*> (ucontext);
#if defined(__x86_64__)
    return static_cast<std::uint64_t> (uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__i386__)
    return static_cast<std::uint64_t> (uc->uc_mcontext.gregs[REG_EIP]);
#elif defined(__aarch64__)
    return static_cast<std::uint64_t> (uc->uc_mcontext.pc);
#elif defined(__arm__)
    return static_cast<std::uint64_t> (uc->uc_mcontext.arm_pc);
#else
    (void)uc;
    return 0;
#endif
  }

  void
  fault_handler (int sig, siginfo_t* info, void* ucontext)
  {
    port_fatal_exception (static_cast<std::uint64_t> (sig), 0,
                          fault_pc (ucontext),
                          reinterpret_cast<std::uint64_t> (
                              info != nullptr ? info->si_addr : nullptr),
                          0);
  }
} /* anonymous namespace */

extern "C" void
port_fatal_exception (std::uint64_t type, std::uint64_t esr,
                      std::uint64_t elr, std::uint64_t far,
                      std::uint64_t spsr)
{
  (void)esr;
  (void)spsr;

  emit ("\n!!! FATAL EXCEPTION on CPU ");
  {
    const unsigned cpu = port_cpu_id ();
    char c = static_cast<char> ('0' + (cpu % 10));
    ssize_t r = ::write (2, &c, 1);
    (void)r;
  }
  emit (" -- signal ");
  emit (::strsignal (static_cast<int> (type)));
  emit (" at ");
  emit_hex (far);
  emit (", pc ");
  emit_hex (elr);
  emit (" (static ");
  emit_hex (elr - g_load_bias);
  emit (")\n");

  /*
   * The scheduler state, which is the whole point of reporting at all: on a
   * machine with several CPUs, "it crashed" says nothing without knowing
   * which thread each CPU was running and who held the kernel lock. The ARM
   * ports print the same two facts from their claim tripwire.
   */
  for (unsigned c = 0; c < OS_NCPU; ++c)
    {
      emit ("  cpu");
      {
        char d = static_cast<char> ('0' + (c % 10));
        ssize_t r = ::write (2, &d, 1);
        (void)r;
      }
      emit (": ");
      os::rtos::thread* th = host_cpu::current_thread (c);
      emit (th == nullptr ? "(null)"
                          : (th->name () != nullptr ? th->name () : "(anon)"));
      emit ("\n");
    }
  emit ("  klock: owner=");
  emit_hex (os::rtos::port::scheduler::_smp_klock.owner);
  emit (" depth=");
  emit_hex (os::rtos::port::scheduler::_smp_klock.depth);
  emit (" lock=");
  emit_hex (os::rtos::port::scheduler::_smp_klock.lock);
  emit ("\n");

  /*
   * The call chain. Not async-signal-safe in the strict sense (backtrace()
   * may take the loader lock on its first call), but the process is already
   * dead and a chain of return addresses is what turns "a null pointer was
   * dereferenced" into "this line dereferenced it".
   */
  {
    void* frames[24];
    int n = ::backtrace (frames, 24);
    emit ("  backtrace (static):\n");
    for (int i = 0; i < n; ++i)
      {
        emit ("    ");
        emit_hex (reinterpret_cast<std::uint64_t> (frames[i]) - g_load_bias);
        emit ("\n");
      }
  }

  // Re-raise with the handler removed, so the shell sees the real cause and a
  // core file is still produced.
  ::signal (static_cast<int> (type), SIG_DFL);
  ::raise (static_cast<int> (type));
  ::_exit (128 + static_cast<int> (type));
}

namespace exception
{
  void
  init ()
  {
    {
      Dl_info info;
      if (::dladdr (reinterpret_cast<void*> (&port_fatal_exception), &info)
          != 0)
        {
          g_load_bias = reinterpret_cast<std::uint64_t> (info.dli_fbase);
        }
    }

    struct sigaction sa; // `sigaction` is also a function: the tag is required
    std::memset (&sa, 0, sizeof (sa));
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    ::sigemptyset (&sa.sa_mask);
    sa.sa_sigaction = fault_handler;

    // SA_ONSTACK here, and NOT on the tick -- the two are opposite cases and
    // the difference matters.
    //
    // The tick must run on the thread's own stack, so its frame migrates with
    // the thread when another CPU resumes it (host_cpu.cpp). A fault handler
    // never migrates: it reports and the process ends. And it must not need
    // the faulting stack, because a blown or wild stack is precisely one of
    // the faults worth reporting -- without an alternate stack that case
    // cannot be reported at all, which is how the first SMP fault found here
    // came out as a silent exit 139.
    //
    // The stack itself is per CPU and installed by host_cpu when the CPU
    // starts, because sigaltstack() is per host thread.
    for (int sig : { SIGSEGV, SIGBUS, SIGFPE, SIGILL })
      {
        ::sigaction (sig, &sa, nullptr);
      }
  }
} /* namespace exception */

#endif /* defined(__APPLE__) || defined(__linux__) */
