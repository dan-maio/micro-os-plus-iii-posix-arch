/**
 * @file smp.hpp
 * @brief Secondary-CPU release for the `native` board.
 * @details The silicon boards release their secondary cores through a spin
 *          table and a mailbox doorbell; this one creates host threads. Same
 *          names, same order, same meaning of g_core_stage[], so the shared
 *          test sources -- which wait for every core to reach stage 3 before
 *          they start timing anything -- compile and behave unchanged.
 */
#pragma once

#include <cstdint>
#include <cmsis-plus/rtos/port/os-decls.h>

extern "C" {

// Per-CPU join/heartbeat status, with the same meaning as on the ARM boards:
//   0 = not started
//   3 = scheduler entered
// Written by the CPU that reaches the stage, read by the tests.
extern volatile std::uint32_t g_core_stage[OS_NCPU];

}

namespace smp {

// Release CPUs 1..OS_NCPU-1 and wait until each has an index and somewhere to
// save a context. On this board that is pthread_create(); on the Pi it is the
// spin table plus SEV. The tests call it at the same point either way.
void start_secondary_cores();

}

// Per-project: create the per-CPU idle threads for CPUs 1..OS_NCPU-1.
extern "C" void smp_install_boot_threads(void);
