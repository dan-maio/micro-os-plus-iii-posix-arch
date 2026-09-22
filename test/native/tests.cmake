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
  if (_app STREQUAL "mutex-stress")
    set (${_out} 1 PARENT_SCOPE)
  else ()
    set (${_out} "${UOS_BOARD_NCPU}" PARENT_SCOPE)
  endif ()
endfunction ()

# ... and it takes none of the board's shared test support, because that
# support is the SMP boot helper: test-smp-boot.cpp installs a per-core idle
# thread through scheduler::os_idle_thread_core[], which exists only under the
# SMP scheduler.
set (BOARD_TEST_SELF_CONTAINED mutex-stress)
