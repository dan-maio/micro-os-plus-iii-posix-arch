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
# Dependencies: sibling clones by default. Override either with -D to build
# against a working copy somewhere else.
get_filename_component (_uos_siblings "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

set (UOS_SMP_DIR "${_uos_siblings}/micro-os-plus-iii-smp"
     CACHE PATH "µOS++ III SMP kernel working copy")
set (UOS_DEVICES_DIR "${_uos_siblings}/micro-os-plus-iii-devices"
     CACHE PATH "µOS++ III devices working copy")

foreach (_dep_var IN ITEMS UOS_SMP_DIR UOS_DEVICES_DIR)
  if (NOT EXISTS "${${_dep_var}}/CMakeLists.txt")
    string (REPLACE "UOS_" "" _repo "${_dep_var}")
    string (REPLACE "_DIR" "" _repo "${_repo}")
    string (TOLOWER "${_repo}" _repo)
    message (FATAL_ERROR
      "Cannot find the ${_repo} repository at ${${_dep_var}}.\n"
      "This project keeps no copy of it. Clone it beside this one:\n"
      "    git clone <remote>/micro-os-plus-iii-${_repo}.git\n"
      "or point at an existing working copy:\n"
      "    cmake -D${_dep_var}=/path/to/it ...")
  endif ()
endforeach ()

add_subdirectory ("${UOS_SMP_DIR}" micro-os-plus-iii-smp)
add_subdirectory ("${UOS_DEVICES_DIR}" micro-os-plus-iii-devices)

# -----------------------------------------------------------------------------
# Boards.
#
# Boards are DISCOVERED, not listed, exactly as in the other three projects:
# every directory under test/boards/ holding a board.cmake is a board, and
# that file is the only place its facts are written. Nothing here knows any
# board's name.
#
# A host "board" is a smaller thing than a silicon one -- it declares a CPU
# count, a console and its capabilities -- but it is the same contract, and it
# is checked by the same loop.
# -----------------------------------------------------------------------------
file (GLOB _board_decls CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/test/boards/*/board.cmake")
set (_board_ids "")
foreach (_decl IN LISTS _board_decls)
  get_filename_component (_dir "${_decl}" DIRECTORY)
  get_filename_component (_id "${_dir}" NAME)
  list (APPEND _board_ids "${_id}")
endforeach ()
list (SORT _board_ids)

set (BOARD "native" CACHE STRING "Target board (one of: ${_board_ids})")
set_property (CACHE BOARD PROPERTY STRINGS ${_board_ids})

if (NOT BOARD IN_LIST _board_ids)
  message (FATAL_ERROR
    "BOARD=${BOARD} is not a board of this port.\n"
    "Available: ${_board_ids}\n"
    "A board is a directory under test/boards/ containing a board.cmake.")
endif ()

include ("${CMAKE_CURRENT_SOURCE_DIR}/test/boards/${BOARD}/board.cmake")

# UOS_BOARD_LINKER_HW is NOT required here, and its absence is the point: a
# host executable is linked by the host toolchain with the host's own script.
foreach (_required IN ITEMS UOS_BOARD_SRC_DIR UOS_BOARD_NCPU UOS_BOARD_CAPS
                            UOS_BOARD_DEFINES)
  if (NOT DEFINED ${_required})
    message (FATAL_ERROR "test/boards/${BOARD}/board.cmake does not set ${_required}.")
  endif ()
endforeach ()

get_filename_component (UOS_BOARD_SRC_DIR "${UOS_BOARD_SRC_DIR}" ABSOLUTE)
if (NOT IS_DIRECTORY "${UOS_BOARD_SRC_DIR}")
  message (FATAL_ERROR
    "test/boards/${BOARD}/board.cmake points at ${UOS_BOARD_SRC_DIR}, which does not exist.")
endif ()

foreach (_cap_lib IN ITEMS sdcard usb-device)
  if ("${_cap_lib}" IN_LIST UOS_BOARD_CAPS AND NOT DEFINED UOS_BOARD_DEVICES)
    message (FATAL_ERROR
             "test/boards/${BOARD}/board.cmake declares the `${_cap_lib}` capability "
             "but no UOS_BOARD_DEVICES to provide it.")
  endif ()
endforeach ()

foreach (_v IN ITEMS UOS_BOARD_SRC_DIR UOS_BOARD_NCPU UOS_BOARD_CAPS)
  set (${_v} "${${_v}}" CACHE INTERNAL "board fact")
endforeach ()

file (GLOB _board_sources CONFIGURE_DEPENDS "${UOS_BOARD_SRC_DIR}/src/*.cpp")

# -----------------------------------------------------------------------------
# The port itself.
add_library (micro-os-plus-iii-posix-arch-interface INTERFACE)

target_include_directories (
  micro-os-plus-iii-posix-arch-interface
  INTERFACE "include" "${UOS_BOARD_SRC_DIR}/include"
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
        # Per-board: the console, the LED, the secondary-CPU release.
        ${_board_sources}
)

target_compile_definitions (
  micro-os-plus-iii-posix-arch-interface
  INTERFACE
        # The port's headers refuse to compile without it, by design:
        # ucontext is an X/Open interface and the declarations differ without
        # it, which is a miscompile rather than an error.
        _XOPEN_SOURCE=700
        _GNU_SOURCE
        ${UOS_BOARD_DEFINES}
)

target_compile_options (
  micro-os-plus-iii-posix-arch-interface INTERFACE ${UOS_BOARD_FLAGS}
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
        ${UOS_BOARD_LIBS}
)

add_library (micro-os-plus::posix-arch ALIAS micro-os-plus-iii-posix-arch-interface)

# -----------------------------------------------------------------------------
if (PROJECT_IS_TOP_LEVEL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
  add_subdirectory (test)
endif ()

message (STATUS "µOS++ III POSIX port: micro-os-plus::posix-arch")
