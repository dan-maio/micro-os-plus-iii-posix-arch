# -----------------------------------------------------------------------------
# micro-os-plus-iii-posix-arch -- the synthetic POSIX SMP port of µOS++ III.
#
# An independent project, laid out exactly like the three cross-compiled ones.
# It consumes the kernel and the device drivers; neither of them knows this
# project exists, so the dependency runs one way and the kernel tree stays
# merge-clean against upstream.
#
# It holds no copy of them. Clone the two repositories beside this one:
#
#   git clone .../micro-os-plus-iii-smp.git
#   git clone .../micro-os-plus-iii-devices.git
#   git clone .../micro-os-plus-iii-posix-arch.git
#
# and they are found automatically as siblings.
#
# What is different here, and only this: the host is the machine. There is no
# linker script, no QEMU and no hardware -- a test IS a host executable, so a
# board builds ONE image per test rather than two, the same way the Luckfox
# Lyra does for the opposite reason.
#
# Exports: micro-os-plus::posix-arch
# -----------------------------------------------------------------------------
cmake_minimum_required (VERSION 3.20)

project (micro-os-plus-iii-posix-arch LANGUAGES C CXX)

set (UOS_POSIX_ARCH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")

# -----------------------------------------------------------------------------
# The kernel (micro-os-plus::iii and its parts) is not added here. Like every
# µOS++ port, this is a leaf library: the consumer (the tests, an application)
# adds the kernel. The targets below link kernel targets by name, which CMake
# resolves when it generates, so the kernel may be added after the port.

# -----------------------------------------------------------------------------
# The host-file SD card: the card driver (sd::SdCard over the file
# UOS_SD_IMAGE), for a consumer that puts its own file system on top (the
# posix-io block device of micro-os-plus-iii-devices-storage, then
# @xpacks/chan-fatfs).
if (NOT TARGET micro-os-plus::devices-hostfile-sd)
  add_library (micro-os-plus-iii-devices-hostfile-sd-interface INTERFACE)

  target_include_directories (
    micro-os-plus-iii-devices-hostfile-sd-interface
    INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/soc/native/include"
  )

  target_compile_definitions (
    micro-os-plus-iii-devices-hostfile-sd-interface INTERFACE SD_BACKEND_HOSTFILE
  )

  target_sources (
    micro-os-plus-iii-devices-hostfile-sd-interface
    INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/soc/native/src/sd_hostfile.cpp"
  )

  add_library (micro-os-plus::devices-hostfile-sd ALIAS
               micro-os-plus-iii-devices-hostfile-sd-interface)
endif ()

# -----------------------------------------------------------------------------
# Boards.
#
# The port is architecture only; it reads no board. A board's facts (CPU
# count, console, capabilities, defines) and its code are added by the
# consumer: the test platforms of micro-os-plus-iii (tests/platforms/<P>/) put
# them in a `micro-os-plus::board` target, as the upstream platforms do.
# -----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
# Sanitizers.
#
# The one thing a synthetic host ought to be better at than silicon, and the
# reason this option exists at all: the same kernel sources, run under a
# checker that no Cortex-A53 can offer.
#
#     -DUOS_SANITIZE=address
#     -DUOS_SANITIZE=undefined
#     -DUOS_SANITIZE=address,undefined
#
# given to cmake when configuring a native test build folder.
#
# What each one is worth here is measured in docs/posix-arch-port.md §21.  The
# short version, from running the whole native suite under each:
#
#   address    11/11 pass, no reports.  Usable as a gate.  It found a real
#              bug on its first run (a dangling thread name in
#              smp-pro-cons-test) that had shipped in every copy of that
#              test.  The port is annotated for it with the ASan fiber API,
#              without which every context switch is a false positive.
#   undefined  11/11 pass, 29 reports, all of them -fsanitize=vptr on the
#              kernel's intrusive-list sentinel downcasts (os-lists.cpp,
#              os-core.cpp), which are deliberate and already carry
#              -W pragmas upstream.  UOS_SANITIZE_NO_VPTR=ON drops them and
#              leaves the suite clean; everything else UBSan checks is clean
#              as it stands.
#   thread     builds, runs, and the test still reaches RESULT: PASS, but it
#              produces ~6000 warnings and a 350k-line log: TSan has no idea
#              a µOS++ thread migrates between host threads.  It has a fiber
#              API (__tsan_create_fiber / __tsan_switch_to_fiber) that looks
#              like the fix, and is not: that model is N:1, many fibers on ONE
#              host thread, and this port is M:N.  The annotation was written
#              and it crashes.  tools/tsan-fiber-probe.c reproduces it in
#              forty lines with no µOS++ in them.  Not a gate, and not
#              something waiting to be done here -- see docs Section 21.4.
#
# Debug or RelWithDebInfo is wanted with any of them, for the line numbers.
set (UOS_SANITIZE "" CACHE STRING
     "Sanitizers to build with: address, undefined, address,undefined, thread")
option (UOS_SANITIZE_NO_VPTR
        "With UOS_SANITIZE=undefined, drop the vptr check (see above)" OFF)

set (_uos_sanitize_opts "")
if (UOS_SANITIZE)
  set (_uos_sanitize_opts
       "-fsanitize=${UOS_SANITIZE}"
       -fno-omit-frame-pointer)
  # This has to come after -fsanitize=, the last flag on the line wins.
  if (UOS_SANITIZE_NO_VPTR)
    list (APPEND _uos_sanitize_opts -fno-sanitize=vptr)
  endif ()
  message (STATUS "µOS++ III POSIX port: -fsanitize=${UOS_SANITIZE}")
endif ()

# -----------------------------------------------------------------------------
# The port itself.
add_library (micro-os-plus-iii-posix-arch-interface INTERFACE)

target_include_directories (
  micro-os-plus-iii-posix-arch-interface
  INTERFACE "include"
)

target_sources (
  micro-os-plus-iii-posix-arch-interface
  INTERFACE
        # The port's half of the scheduler. The kernel's own
        # src/rtos/os-core.cpp is the portable half and is compiled alongside.
        src/rtos/os-core.cpp
        # The CPU model: host threads, per-CPU timers, the IPI.
        src/host_cpu.cpp
        # A synchronous fault is a signal on this machine; report it.
        src/exception_handler.cpp
        # os_startup_initialize_free_store(): the kernel's own copy is
        # __ARM_EABI__-only, so the port supplies it. See the file.
        src/free-store.cpp
        # Carried unchanged from upstream v1.0.1.
        src/diag/trace-posix.cpp
        # No code. Fails the build the moment a board has not stated
        # something the shared sources need.
        src/board-contract.cpp
)

target_compile_definitions (
  micro-os-plus-iii-posix-arch-interface
  INTERFACE
        # The port's headers refuse to compile without it, by design:
        # ucontext is an X/Open interface and the declarations differ without
        # it, which is a miscompile rather than an error.
        _XOPEN_SOURCE=700
        _GNU_SOURCE
)

target_compile_options (
  micro-os-plus-iii-posix-arch-interface
  INTERFACE ${_uos_sanitize_opts}
)

target_link_options (
  micro-os-plus-iii-posix-arch-interface INTERFACE ${_uos_sanitize_opts}
)

target_link_libraries (
  micro-os-plus-iii-posix-arch-interface
  INTERFACE
        micro-os-plus::iii
        # NOT micro-os-plus::port-smp-decls. That target's own README gives
        # this port as the example of one that must keep its own os-decls.h:
        # its state types are not integers and its thread context is not a
        # bare stack pointer. See include/cmsis-plus/rtos/port/os-decls.h.
        pthread
        rt
)

add_library (micro-os-plus::posix-arch ALIAS micro-os-plus-iii-posix-arch-interface)

# Upstream's name for the same target; kept so the upstream platform files
# that still use it keep configuring.
add_library (micro-os-plus::iii-posix-arch ALIAS micro-os-plus-iii-posix-arch-interface)

message (STATUS "µOS++ III POSIX port: micro-os-plus::posix-arch")
