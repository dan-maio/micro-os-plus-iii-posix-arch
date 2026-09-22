/**
 * @file exception_handler.hpp
 * @brief Synchronous-fault reporting for the POSIX port.
 *
 * On the ARM ports a synchronous exception lands in a vector installed by
 * startup.S and is reported by port_fatal_exception(). The host equivalent of
 * a synchronous exception is a signal -- SIGSEGV, SIGBUS, SIGFPE, SIGILL --
 * so exception::init() installs handlers for those and reports through the
 * same function, with the same name, so a test that faults says so instead of
 * dying silently with no output.
 *
 * This matters more here than on the silicon, not less: a host process that
 * segfaults under a test runner produces an exit status and nothing else, and
 * with several CPUs running there is no way afterwards to tell which one
 * faulted or in which thread.
 */
#pragma once

#include <cstdint>

namespace exception {

void init();

} // namespace exception

extern "C" void port_fatal_exception(std::uint64_t type, std::uint64_t esr,
                                     std::uint64_t elr, std::uint64_t far,
                                     std::uint64_t spsr);
