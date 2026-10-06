/*
 * smp.cpp - the `native` board's secondary-CPU release.
 *
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 *
 * The board half is thin here, and deliberately so. On the BCM2837 the
 * equivalent file owns the spin table, the mailbox doorbell and the cache
 * maintenance around them, because releasing a core is a silicon act. On this
 * machine the silicon is the host kernel, so the act belongs to the port
 * (host_cpu) and the board only names it -- which is the same division of
 * labour, applied honestly rather than copied.
 */

#include <smp.hpp>
#include <host_cpu.hpp>

extern "C" {
volatile std::uint32_t g_core_stage[OS_NCPU] = {};
}

namespace smp {

void start_secondary_cores()
{
    host_cpu::start_secondary_cpus();
}

} // namespace smp
