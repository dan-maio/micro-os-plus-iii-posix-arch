/*
 * free-store.cpp - the application free store on the POSIX host.
 *
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) 2016-2025 Liviu Ionescu. All rights reserved.
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 *
 * ---------------------------------------------------------------------------
 * Why this file exists at all, and why it does NOT just call malloc().
 *
 * The kernel's own src/startup/initialise-free-store.cpp is guarded by
 * `#if defined(__ARM_EABI__)` from top to bottom, so on a host it compiles to
 * nothing and os_startup_initialize_free_store() is undefined -- which is
 * exactly what the carried tests call. Relaxing that guard would mean editing
 * the kernel, which is kept merge-clean against upstream; so the port
 * supplies the hook, which is what a port is for.
 *
 * Upstream's posix-arch had no such hook and let the system malloc() serve,
 * and its NOTES.md says so. That was correct for upstream, which had ONE host
 * thread. It is not correct here, and the difference is the whole subject of
 * this port:
 *
 *   A µOS++ thread may be preempted inside an allocation and resumed on a
 *   DIFFERENT CPU -- that is, a different host thread. glibc's malloc is
 *   thread-safe by taking an arena lock, and that lock would then be released
 *   by a host thread that did not take it. Nothing in the C library promises
 *   that works.
 *
 * Using µOS++'s own memory resource removes the question rather than betting
 * on it: the allocator is the kernel's, its mutual exclusion is the kernel's
 * scheduler lock, and the scheduler lock already migrates correctly because
 * making it do so is what the rest of this port is about. It also makes the
 * host behave like the three silicon ports, which is the point of testing on
 * it -- a first_fit_top over a fixed block is what every board does, so an
 * allocation pattern that exhausts the heap here exhausts it there too,
 * instead of being quietly absorbed by a host with gigabytes to spare.
 * ---------------------------------------------------------------------------
 */

#if defined(__APPLE__) || defined(__linux__)

#include <cstddef>
#include <new>

#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/rtos/os-hooks.h>
#include <cmsis-plus/memory/first-fit-top.h>
#include <cmsis-plus/estd/memory_resource>

using namespace os;

#if defined(OS_TYPE_APPLICATION_MEMORY_RESOURCE)
using application_memory_resource = OS_TYPE_APPLICATION_MEMORY_RESOURCE;
#else
using application_memory_resource = os::memory::first_fit_top;
#endif

namespace
{
  // Same storage trick as the kernel's ARM version: the resource itself must
  // outlive everything and must not need the free store to exist first.
  alignas (application_memory_resource) char
      application_free_store[sizeof (application_memory_resource)];
} // namespace

/*
 * The out-of-memory hooks, for the same reason as the hook above: the
 * kernel's definitions live in the __ARM_EABI__-guarded file. Weak, so an
 * application that wants to reset the machine, dump the heap or coalesce
 * before giving up replaces them by defining its own -- which is exactly the
 * contract they have on the silicon ports.
 */
void __attribute__ ((weak))
os_rtos_application_out_of_memory_hook (void)
{
  estd::__throw_bad_alloc ();
}

#if defined(OS_INTEGER_RTOS_DYNAMIC_MEMORY_SIZE_BYTES)

void __attribute__ ((weak))
os_rtos_system_out_of_memory_hook (void)
{
  estd::__throw_bad_alloc ();
}

#endif /* defined(OS_INTEGER_RTOS_DYNAMIC_MEMORY_SIZE_BYTES) */

/*
 * Weak no-ops, so an application that brings up no hardware still links.
 * Every carried test defines both, and its definitions win.
 */
void __attribute__ ((weak))
os_startup_initialize_hardware_early (void)
{
}

void __attribute__ ((weak))
os_startup_initialize_hardware (void)
{
}

void __attribute__ ((weak))
os_startup_initialize_free_store (void* heap_address,
                                  std::size_t heap_size_bytes)
{
#if !defined(OS_EXCLUDE_DYNAMIC_MEMORY_ALLOCATIONS)

  new (&application_free_store)
      application_memory_resource{ "app-heap", heap_address,
                                   heap_size_bytes };

  reinterpret_cast<rtos::memory::memory_resource*> (&application_free_store)
      ->out_of_memory_handler (os_rtos_application_out_of_memory_hook);

  estd::pmr::set_default_resource (
      reinterpret_cast<estd::pmr::memory_resource*> (&application_free_store));

  // No sbrk() adjustment. On a bare-metal target the kernel's version pushes
  // sbrk past the free store so newlib's malloc cannot collide with it; here
  // the block is ordinary static storage the host already owns, so there is
  // nothing to push it past.

#else
  (void)heap_address;
  (void)heap_size_bytes;
#endif /* !defined(OS_EXCLUDE_DYNAMIC_MEMORY_ALLOCATIONS) */
}

#endif /* defined(__APPLE__) || defined(__linux__) */
