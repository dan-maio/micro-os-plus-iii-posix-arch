# -----------------------------------------------------------------------------
# native — the POSIX host, OS_NCPU host threads standing in for cores.
#
# One file per board, discovered by the presence of this file. Nothing in the
# port's own CMakeLists knows this board exists.
#
# Two board facts that the silicon boards have and this one has not, and the
# absence is the statement:
#
#   UOS_BOARD_LINKER_HW    — the host toolchain links with its own script.
#   UOS_BOARD_LINKER_QEMU  — the host IS the machine; there is nothing to
#                            emulate. A test therefore builds ONE image, the
#                            way the hardware-only Luckfox Lyra does, for the
#                            mirror-image reason.
# -----------------------------------------------------------------------------

set (UOS_BOARD_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}")

# No -mcpu: the host compiler targets the host.
set (UOS_BOARD_FLAGS "")

# How many CPUs this board has. It is a board fact here exactly as it is on
# the silicon boards -- but it is the one board fact on this machine that can
# honestly be changed from the command line, because the "silicon" is a
# thread count. -DNCPU=1 builds the whole suite single-core, which is the
# first clause of step 5's gate.
set (NCPU 4 CACHE STRING "Host threads acting as CPUs")
set (UOS_BOARD_NCPU ${NCPU})

# What this board can do. The test matrix asks about capabilities; it never
# asks which board it is building for.
#
# `sdcard` is claimed because the host file backend below IS a block device
# the SD tests can reach -- not because the machine has a slot. `usb-device`
# is not claimed: nothing here can be a USB gadget, and saying so would make
# usb_test appear and fail.
set (UOS_BOARD_CAPS  smp sdcard led)

set (UOS_BOARD_LIBS  "")

# The driver set an application links when it needs the SD card: the host
# file backend, which is micro-os-plus-iii-devices' third one after the
# BCM2837 SDHCI and the RK3506 DW-MMC.
set (UOS_BOARD_DEVICES  micro-os-plus::devices-hostfile)

# SD_BACKEND_HOSTFILE is NOT here: it belongs to the devices target that
# provides that back end, exactly as SD_BACKEND_DWMMC belongs to the RK3506
# one. A board names the driver set; the driver set names itself.
set (UOS_BOARD_DEFINES
     # No CPU count in the text. The board's NCPU is the default, but a test
     # may build at another (mutex-stress builds at 1, through board_test_ncpu),
     # and a banner that named the board's number would be wrong for it. The
     # count is printed by port::scheduler::greeting(), from OS_NCPU, which is
     # per-test and therefore always right.
     "PORT_GREETING=\"µOS++ POSIX synthetic host (host threads as CPUs)\"")
