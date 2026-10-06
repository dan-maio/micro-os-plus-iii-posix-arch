# -----------------------------------------------------------------------------
# Test builds for the POSIX port.
#
# The same loop as the three cross-compiled projects, with one difference, and
# it is a difference in the machine rather than in the method:
#
#   ONE VARIANT, not two. The other ports build each test twice -- `qemu` for
#   the emulated suite and `hwd` for the probe -- because the image that runs
#   under an emulator is not the image that runs on the silicon. Here the host
#   IS the machine. There is nothing to emulate and nothing to flash, so there
#   is one image, named `host`, and a test is simply an executable.
#
#   It is NOT a HW_BUILD. That define means "use the board's real FAT32 boot
#   partition"; the SD tests' other branch is flatfs over a plain image file,
#   which is exactly what a host file backend is. Claiming HW_BUILD here would
#   silently point the SD tests at a partition that does not exist.
#
# Everything else is unchanged: a board builds the directories it has, there
# is no shared test list and no capability filter, and the per-test knobs come
# from the board's own tests.cmake through the same six hooks.
# -----------------------------------------------------------------------------

set (_test_dir "${CMAKE_CURRENT_SOURCE_DIR}/${BOARD}")

if (NOT IS_DIRECTORY "${_test_dir}")
  message (FATAL_ERROR
           "test/${BOARD} does not exist. Every board has a directory here -- "
           "it is where that board's test applications live, even when the "
           "board shares its src/ and include/ with another.")
endif ()

set (_variants host)

set (_common_defines TRACE OS_USE_TRACE_POSIX_STDOUT)

# The knobs a directory listing cannot express. Optional: a board whose tests
# need nothing beyond the defaults has no tests.cmake. The hooks are the same
# six the other three projects define, with the same defaults, so a test may
# be carried between ports without its build rules changing meaning.
set (BOARD_TEST_NEED_DEVICES "")
set (BOARD_TEST_SELF_CONTAINED "")
set (BOARD_TEST_NO_KERNEL "")
function (board_test_defines _app _out)
  set (${_out} "" PARENT_SCOPE)
endfunction ()
function (board_test_sources _app _out)
  set (${_out} "" PARENT_SCOPE)
endfunction ()
function (board_test_includes _app _out)
  set (${_out} "" PARENT_SCOPE)
endfunction ()
# Kept for symmetry with the other three projects, and it does nothing here:
# a host executable is linked by the host toolchain with its own script, and
# a test that tried to name one would be describing a machine it is not on.
function (board_test_linker _app _out)
  set (${_out} "" PARENT_SCOPE)
endfunction ()
function (board_test_options _app _out)
  set (${_out} "" PARENT_SCOPE)
endfunction ()
function (board_test_ncpu _app _out)
  set (${_out} "${UOS_BOARD_NCPU}" PARENT_SCOPE)
endfunction ()
# A SEVENTH hook, which the other three projects do not have yet.
#
# The six above are shared, deliberately, so a test may be carried between
# ports without its build rules changing meaning. This one is here because
# this port needed it first: rtos-apis is upstream's API suite and links
# micro-os-plus::iii-posix-io, which is an opt-in target of the kernel rather
# than a device driver -- so UOS_BOARD_DEVICES, which names a board's driver
# set, is the wrong place to ask for it.
#
# It defaults to nothing, so adding it to the other three ports would change
# no generated build. That is a separate change and it needs their
# regressions, so it has not been made.
function (board_test_libraries _app _out)
  set (${_out} "" PARENT_SCOPE)
endfunction ()
if (EXISTS "${_test_dir}/tests.cmake")
  include ("${_test_dir}/tests.cmake")
endif ()

# The support this board's tests share: one copy per board, not per test.
file (GLOB _test_support CONFIGURE_DEPENDS "${_test_dir}/src/*.cpp")
set (_test_includes "${_test_dir}/include")

file (GLOB _test_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS "${_test_dir}/*")

set (_apps "")
foreach (_dir IN LISTS _test_dirs)
  if (NOT IS_DIRECTORY "${_dir}")
    continue ()
  endif ()
  get_filename_component (_app "${_dir}" NAME)
  if (_app STREQUAL "include" OR _app STREQUAL "src")
    continue ()
  endif ()
  file (GLOB _srcs CONFIGURE_DEPENDS "${_dir}/*.cpp")
  if (NOT _srcs)
    continue ()
  endif ()
  list (APPEND _apps "${_app}")

  board_test_defines  ("${_app}" _app_defines)
  board_test_sources  ("${_app}" _app_sources)
  board_test_includes ("${_app}" _app_includes)
  board_test_options  ("${_app}" _app_options)
  board_test_ncpu     ("${_app}" _app_ncpu)
  board_test_libraries ("${_app}" _app_libs)

  set (_app_support ${_test_support})
  if ("${_app}" IN_LIST BOARD_TEST_SELF_CONTAINED)
    set (_app_support "")
  endif ()

  if (_app_options)
    set_source_files_properties (${_srcs} PROPERTIES COMPILE_OPTIONS "${_app_options}")
  endif ()

  set (_libs micro-os-plus::posix-arch)
  set (_no_kernel "")
  if ("${_app}" IN_LIST BOARD_TEST_NO_KERNEL)
    if (NOT UOS_PORT_BARE_LIB)
      message (FATAL_ERROR
               "${_app} is listed in BOARD_TEST_NO_KERNEL, but this port "
               "declares no bare target (UOS_PORT_BARE_LIB).")
    endif ()
    set (_libs ${UOS_PORT_BARE_LIB})
    set (_no_kernel NO_KERNEL)
  endif ()
  if ("${_app}" IN_LIST BOARD_TEST_NEED_DEVICES)
    list (APPEND _libs ${UOS_BOARD_DEVICES})
  endif ()
  if (_app_libs)
    list (APPEND _libs ${_app_libs})
  endif ()

  foreach (_variant IN LISTS _variants)
    uos_add_app (
      "${_app}-${_variant}"
      ${_no_kernel}
      SOURCES ${_srcs} ${_app_support} ${_app_sources}
      NCPU ${_app_ncpu}
      DEFINES ${_common_defines} ${_app_defines}
      INCLUDES ${_app_includes} ${_dir} ${_test_includes}
      LIBRARIES ${_libs}
      OUTPUT_NAME "${_app}-${_variant}"
    )
  endforeach ()
endforeach ()

list (LENGTH _apps _n)
list (LENGTH _variants _nv)
math (EXPR _n "${_n} * ${_nv}")
message (STATUS "µOS++ III POSIX tests: ${_n} targets, board=${BOARD}, "
                "OS_NCPU=${UOS_BOARD_NCPU}, from ${_test_dir}")
