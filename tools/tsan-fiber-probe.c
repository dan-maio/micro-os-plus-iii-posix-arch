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

/*
 * Does this toolchain's ThreadSanitizer support a fibre that MIGRATES
 * between host threads?
 *
 *     cc -fsanitize=thread -g -O1 -o tsan-fiber-probe \
 *        tools/tsan-fiber-probe.c -lpthread && ./tsan-fiber-probe
 *
 *     prints "done"                -> yes; the port could be annotated
 *     CHECK failed: tsan_rtl_proc  -> no; see docs/posix-arch-port.md §21.4
 *
 * Why this file exists
 * --------------------
 * TSan reports thousands of false races on this port because it tracks
 * happens-before per HOST thread, and a µOS++ thread is not one: it shares a
 * host thread with others and it migrates between them. TSan publishes a
 * fibre API that is meant to fix exactly that, and the port was annotated for
 * it -- __tsan_create_fiber() per thread, __tsan_switch_to_fiber() before
 * every swapcontext(), the same shape of work already done for ASan.
 *
 * It crashed, non-deterministically, at OS_NCPU=1 and at OS_NCPU=4, with and
 * without the signal-handler switches. This probe is what established that
 * the fault is not in the port: it contains no µOS++ at all, it is forty
 * lines, and it fails the same way. TSan's fibre model is N:1 -- many fibres
 * on ONE host thread -- and resuming a fibre on a different host thread trips
 * an internal assertion in ProcWire().
 *
 * Keep it. When the toolchain changes, this answers in one second whether the
 * annotation is worth attempting again, and the annotation itself is
 * described in full in docs/posix-arch-port.md §21.4.
 */

#define _GNU_SOURCE

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <ucontext.h>

/* Declared by hand: <sanitizer/tsan_interface.h> is not installed by every
 * toolchain, and the symbols come from the TSan runtime. */
void*
__tsan_create_fiber (unsigned flags);
void
__tsan_switch_to_fiber (void* fiber, unsigned flags);

static ucontext_t host_uc[2];
static ucontext_t fibre_uc;
static char fibre_stack[256 * 1024];
static void* fibre;

/* 0: not started  1: parked by host A  2: host B may take it  3: finished */
static atomic_int stage = 0;

static void
fibre_body (void)
{
  printf ("  fibre: running, on host A\n");
  atomic_store (&stage, 1);
  swapcontext (&fibre_uc, &host_uc[0]);

  /* The migration. On this port it is the normal case, not the exception. */
  printf ("  fibre: resumed, on host B\n");
  atomic_store (&stage, 3);
  swapcontext (&fibre_uc, &host_uc[1]);
}

static void*
host_b (void* unused)
{
  (void)unused;

  while (atomic_load (&stage) != 2)
    {
      ;
    }

  printf ("host B: taking the fibre over\n");
  __tsan_switch_to_fiber (fibre, 0);
  swapcontext (&host_uc[1], &fibre_uc);
  printf ("host B: fibre handed back\n");

  return NULL;
}

int
main (void)
{
  fibre = __tsan_create_fiber (0);

  getcontext (&fibre_uc);
  fibre_uc.uc_stack.ss_sp = fibre_stack;
  fibre_uc.uc_stack.ss_size = sizeof (fibre_stack);
  fibre_uc.uc_link = NULL;
  makecontext (&fibre_uc, fibre_body, 0);

  pthread_t t;
  pthread_create (&t, NULL, host_b, NULL);

  printf ("host A: starting the fibre\n");
  __tsan_switch_to_fiber (fibre, 0);
  swapcontext (&host_uc[0], &fibre_uc);

  printf ("host A: fibre parked, handing it to host B\n");
  atomic_store (&stage, 2);

  pthread_join (t, NULL);

  printf ("done\n");
  return 0;
}
