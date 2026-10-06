# -----------------------------------------------------------------------------
#
# This file is part of the µOS++ project (https://micro-os-plus.github.io/).
# Copyright (c) 2021-2025 Liviu Ionescu. All rights reserved.
#
# Permission to use, copy, modify, and/or distribute this software
# for any purpose is hereby granted, under the terms of the MIT license.
#
# If a copy of the license was not distributed with this file, it can
# be obtained from https://opensource.org/licenses/mit/.
#
# -----------------------------------------------------------------------------

# This file is intended to be consumed by applications with:
#
# `add_subdirectory("xpacks/@micro-os-plus/micro-os-plus-iii-posix-arch")`
#
# The result is an interface library that can be added to the linker with:
#
# `target_link_libraries(your-target PUBLIC micro-os-plus::iii-posix-arch)`

# -----------------------------------------------------------------------------
## Preamble ##

# https://cmake.org/cmake/help/v3.20/
cmake_minimum_required(VERSION 3.20)

project(micro-os-plus-iii-posix-arch
  DESCRIPTION "µOS++ synthetic POSIX port"
)

if(COMMAND xpack_get_package_name_and_version)
  xpack_get_package_name_and_version("${CMAKE_CURRENT_SOURCE_DIR}/package.json")
  message(VERBOSE "Processing xPack ${PACKAGE_JSON_NAME}@${PACKAGE_JSON_VERSION}...")
endif()

# -----------------------------------------------------------------------------
## The project library definitions ##

add_library(micro-os-plus-iii-posix-arch-interface INTERFACE EXCLUDE_FROM_ALL)

# -----------------------------------------------------------------------------
# Target settings.

target_include_directories(micro-os-plus-iii-posix-arch-interface INTERFACE
  "include"
)

target_sources(micro-os-plus-iii-posix-arch-interface INTERFACE
  src/diag/trace-posix.cpp
  src/rtos/os-core.cpp
  src/host_cpu.cpp
  src/free-store.cpp
  src/exception_handler.cpp
)

target_compile_definitions(micro-os-plus-iii-posix-arch-interface INTERFACE
  # None.
)

target_compile_options(micro-os-plus-iii-posix-arch-interface INTERFACE
  # None.
)

target_link_libraries(micro-os-plus-iii-posix-arch-interface INTERFACE
  # None.
)

if (COMMAND xpack_display_target_lists)
  xpack_display_target_lists(micro-os-plus-iii-posix-arch-interface)
endif()

# -----------------------------------------------------------------------------
# Aliases.

# https://cmake.org/cmake/help/v3.20/command/add_library.html#alias-libraries
add_library(micro-os-plus::iii-posix-arch ALIAS micro-os-plus-iii-posix-arch-interface)
message(VERBOSE "> micro-os-plus::iii-posix-arch -> micro-os-plus-iii-posix-arch-interface")


# Dissolved devices support: hostfile block device, flatfs, and SD stack.
# The devices repository was dissolved into the architecture ports.
if (NOT TARGET micro-os-plus::devices-hostfile)
  add_library (micro-os-plus-iii-devices-hostfile-interface INTERFACE)

  target_include_directories (
    micro-os-plus-iii-devices-hostfile-interface
    INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/soc/native/include"
  )

  target_compile_definitions (
    micro-os-plus-iii-devices-hostfile-interface INTERFACE SD_BACKEND_HOSTFILE
  )

  target_sources (
    micro-os-plus-iii-devices-hostfile-interface
    INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/soc/native/src/sd_hostfile.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/src/flatfs.cpp"
  )

  add_library (micro-os-plus::devices-hostfile ALIAS
               micro-os-plus-iii-devices-hostfile-interface)
endif ()

if (NOT TARGET micro-os-plus::devices)
  add_library (micro-os-plus-iii-devices-interface INTERFACE)

  target_include_directories (
    micro-os-plus-iii-devices-interface
    INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs"
  )

  target_sources (
    micro-os-plus-iii-devices-interface
    INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/src/sd.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/src/flatfs.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/src/usb_dwc2.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs/ff.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs/diskio_sd.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs/fatfs_hw.cpp"
  )

  add_library (micro-os-plus::devices ALIAS
               micro-os-plus-iii-devices-interface)
endif ()

# -----------------------------------------------------------------------------
