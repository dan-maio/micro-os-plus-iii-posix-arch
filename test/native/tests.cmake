# -----------------------------------------------------------------------------
# What this board's tests need.
#
# Which tests exist is the directory listing beside this file -- a board builds
# what it has -- so this file carries only the knobs a test cannot state for
# itself.
# -----------------------------------------------------------------------------

# The single-core leg.
#
# mutex-stress and rtos-apis are upstream's own suites (tests/sources/<suite>,
# the platform's UOS_TESTS_SUITES), built here at OS_NCPU=1 because the nine
# SMP object tests cannot be built at OS_NCPU=1 by
# construction: without OS_USE_SMP_SCHEDULER the kernel's `current_thread_` is
# a scalar and `thread::cpu_affinity()` does not exist, and they use both. The
# port itself is dual-branch -- like cortexm, which runs its three STM32 boards
# at OS_NCPU=1 off the RP2350's SMP core -- and this is what proves that branch
# is built and run rather than merely written.
#
# Their SMP legs are smp-mutex-stress and smp-rtos-apis: the same upstream
# tests at UOS_BOARD_NCPU, each with per-core checks that fail the run unless
# every core did the work. They take the default below.
function (board_test_ncpu _app _out)
  if (_app STREQUAL "mutex-stress" OR _app STREQUAL "rtos-apis")
    set (${_out} 1 PARENT_SCOPE)
  else ()
    set (${_out} "${UOS_BOARD_NCPU}" PARENT_SCOPE)
  endif ()
endfunction ()

# ... and it takes none of the board's shared test support, because that
# support is the SMP boot helper: test-smp-boot.cpp installs a per-core idle
# thread through scheduler::os_idle_thread_core[], which exists only under the
# SMP scheduler.
set (BOARD_TEST_SELF_CONTAINED mutex-stress rtos-apis)

# rtos-apis carries a C source, and the loop globs only *.cpp.
# rtos-apis wants its own os-app-config.h -- the statistics and
# instrumentation switches its sub-tests read are in it.
function (board_test_defines _app _out)
  if (_app STREQUAL "smp-rtos-apis")
    # (The suites get theirs from the builder.)
    set (${_out} OS_USE_OS_APP_CONFIG_H PARENT_SCOPE)
  elseif (_app STREQUAL "rtos-apis")
    # Upstream's suite runs its Chan FatFs cases on RAM disks up to ~34 MB
    # (FAT32: 66130 blocks) on a host; on upstream's native platform `new` is
    # the host's malloc, here the board's free store (boards/native/src/
    # heap.cpp, 32 MiB by default), so this image gets a bigger one.
    set (${_out} UOS_BOARD_HEAP_BYTES=100663296 PARENT_SCOPE) # 96 MiB
  elseif (_app STREQUAL "smp-mat-test")
    # The host has the memory for a bigger system than the boards' default.
    set (${_out} SMP_MAT_N=500 SMP_MAT_B=50 PARENT_SCOPE)
  else ()
    set (${_out} "" PARENT_SCOPE)
  endif ()
endfunction ()

# rtos-apis links the kernel's opt-in POSIX I/O target. Its c-syscalls-posix.cpp
# defines __posix_open/__posix_read/… -- prefixed, so on a host they do not
# collide with glibc's own open/read/write, which the board's console, the
# trace backend and the host-file SD back-end all call.
# The libucontext package's own C sources do not compile with the port's
# _GNU_SOURCE (glibc's REG_* enumeration collides with libucontext's REG_*
# macros); upstream's port does not set it. They are compiled into the
# applications here, in this directory, so their properties are set here.
if (TARGET xpack-3rd-party-libucontext-interface)
  get_target_property (_ucontext_sources xpack-3rd-party-libucontext-interface
                       INTERFACE_SOURCES)
  if (_ucontext_sources)
    set_source_files_properties (${_ucontext_sources} PROPERTIES
                                 COMPILE_OPTIONS "-U_GNU_SOURCE")
  endif ()
endif ()

function (board_test_libraries _app _out)
  # Upstream's suites' configuration (OS_INCLUDE_LIBUCONTEXT) selects the
  # libucontext package, which upstream's native platform links too.
  if (_app STREQUAL "smp-rtos-apis")
    set (${_out} micro-os-plus::iii-posix-io xpack-3rd-party::libucontext
         PARENT_SCOPE)
  elseif (_app STREQUAL "mutex-stress" OR _app STREQUAL "rtos-apis")
    set (${_out} xpack-3rd-party::libucontext PARENT_SCOPE)
  else ()
    set (${_out} "" PARENT_SCOPE)
  endif ()
endfunction ()

function (board_test_sources _app _out)
  if (_app STREQUAL "smp-rtos-apis")
    # The SMP leg carries only its main.cpp; the API tests are upstream's
    # rtos-apis (tests/sources/rtos-apis/src), every source but its main.cpp
    # and the Chan FatFs one, which this leg does not run.
    file (GLOB _api CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/sources/rtos-apis/src/*.cpp")
    list (FILTER _api EXCLUDE REGEX "/(main|test-chan-fatfs)\\.cpp$")
    set (${_out} ${_api} "${CMAKE_SOURCE_DIR}/sources/rtos-apis/src/test-c-api.c"
         PARENT_SCOPE)
  else ()
    set (${_out} "" PARENT_SCOPE)
  endif ()
endfunction ()

# ... and it finds rtos-apis' headers and os-app-config.h where they are,
# upstream's.
function (board_test_includes _app _out)
  if (_app STREQUAL "smp-rtos-apis")
    # (That os-app-config.h includes the platform's cmsis-plus/platform.h.)
    set (${_out} "${CMAKE_SOURCE_DIR}/sources/rtos-apis/include"
         "${CMAKE_SOURCE_DIR}/platforms/${PLATFORM_NAME}/include" PARENT_SCOPE)
  else ()
    set (${_out} "" PARENT_SCOPE)
  endif ()
endfunction ()
