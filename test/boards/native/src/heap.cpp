/*
 * heap.cpp - the `native` board's memory map.
 *
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 *
 * On the silicon boards __heap_start and __heap_end come from the linker
 * script, because where the heap is IS the board's memory map. A host
 * executable has no such script, so the board states the same fact the only
 * way it can.
 *
 * It has to be assembly, and the reason is worth stating because a C++
 * definition looks like it would do and does not. The tests say
 *
 *     extern char __heap_start[]; extern char __heap_end[];
 *     os_startup_initialize_free_store (__heap_start,
 *         static_cast<std::size_t> (__heap_end - __heap_start));
 *
 * so both names must be LABELS whose addresses bracket the block. Defining
 * __heap_end as `char* const` would link -- and then subtract the address of
 * a pointer variable from the address of the array, giving a size that is
 * whatever the linker's layout happened to be. Two labels around one .zero
 * in one section is the only spelling that cannot be wrong.
 *
 * UOS_BOARD_HEAP_BYTES (board.cmake) sizes it. It is a board fact for the
 * same reason the RAM size is one on a Pi -- and a deliberately finite one:
 * a heap that could never run out would make every out-of-memory path in
 * these tests unreachable on the very machine where they are easiest to
 * debug.
 */

#if !defined(UOS_BOARD_HEAP_BYTES)
#define UOS_BOARD_HEAP_BYTES (32 * 1024 * 1024)
#endif

#define UOS_STR_(x) #x
#define UOS_STR(x) UOS_STR_ (x)

#if defined(__ELF__)

__asm__ (".section .bss.uos_heap,\"aw\",@nobits\n"
         ".balign 16\n"
         ".globl __heap_start\n"
         ".hidden __heap_start\n"
         "__heap_start:\n"
         ".zero " UOS_STR (UOS_BOARD_HEAP_BYTES) "\n"
         ".globl __heap_end\n"
         ".hidden __heap_end\n"
         "__heap_end:\n"
         ".size __heap_start, __heap_end - __heap_start\n"
         ".previous\n");

#else
#error "The native board defines its heap with ELF assembly; port it for this object format."
#endif
