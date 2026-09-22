# -----------------------------------------------------------------------------
# What this board's tests need.
#
# Which tests exist is the directory listing beside this file -- a board builds
# what it has -- so this file carries only the knobs a test cannot state for
# itself.
# -----------------------------------------------------------------------------

# Tests that reach a block device, and so link UOS_BOARD_DEVICES. On this
# board that is flatfs over the host file backend: both of these take their
# `!HW_BUILD` branch, which formats a flatfs volume on sd::SdCard rather than
# mounting a FAT32 partition. They are the reason the board declares the
# `sdcard` capability at all.
set (BOARD_TEST_NEED_DEVICES smp-num-test smp-pipeline-test)

# The single-core leg.
#
# mutex-stress is upstream's own test (tests/sources/mutex-stress), carried
# here because the nine SMP object tests cannot be built at OS_NCPU=1 by
# construction: without OS_USE_SMP_SCHEDULER the kernel's `current_thread_` is
# a scalar and `thread::cpu_affinity()` does not exist, and they use both. The
# port itself is dual-branch -- like cortexm, which runs its three STM32 boards
# at OS_NCPU=1 off the RP2350's SMP core -- and this is what proves that branch
# is built and run rather than merely written.
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

# Captured HERE, not inside the function: CMAKE_CURRENT_LIST_DIR is evaluated
# where a function RUNS, which is test/CMakeLists.txt, not where it is written.
set (_native_tests_dir "${CMAKE_CURRENT_LIST_DIR}")

# rtos-apis carries a C source, and the loop globs only *.cpp.
# rtos-apis wants its own os-app-config.h -- the statistics and
# instrumentation switches its sub-tests read are in it.
function (board_test_defines _app _out)
  if (_app STREQUAL "rtos-apis")
    set (${_out} OS_USE_OS_APP_CONFIG_H PARENT_SCOPE)
  else ()
    set (${_out} "" PARENT_SCOPE)
  endif ()
endfunction ()

# rtos-apis links the kernel's opt-in POSIX I/O target. Its c-syscalls-posix.cpp
# defines __posix_open/__posix_read/… -- prefixed, so on a host they do not
# collide with glibc's own open/read/write, which the board's console, the
# trace backend and the host-file SD back-end all call.
function (board_test_libraries _app _out)
  if (_app STREQUAL "rtos-apis")
    set (${_out} micro-os-plus::iii-posix-io PARENT_SCOPE)
  else ()
    set (${_out} "" PARENT_SCOPE)
  endif ()
endfunction ()

function (board_test_sources _app _out)
  if (_app STREQUAL "rtos-apis")
    set (${_out} "${_native_tests_dir}/rtos-apis/test-c-api.c" PARENT_SCOPE)
  else ()
    set (${_out} "" PARENT_SCOPE)
  endif ()
endfunction ()
