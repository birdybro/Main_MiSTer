# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Main_MiSTer is the ARM Linux system software (main binary) for the MiSTer FPGA platform. It manages the interface between FPGA cores and the user — handling core loading, input, video output, file I/O, OSD menus, and core-specific support logic. Written in C (gnu99) and C++ (gnu++14).

## Build Commands

Cross-compiled for ARM using GCC 10.2.1 (`arm-none-linux-gnueabihf`). Output: `bin/MiSTer`.

```bash
make                    # Build (auto-parallel via -j nproc)
make clean              # Clean build artifacts
DEBUG=1 make            # Debug build (-O0 -g instead of -O3)
PROFILING=1 make        # Build with profiling support
V=1 make                # Verbose build output
```

**Toolchain setup:** Run `setup_default_toolchain.sh` or use the devcontainer (`.devcontainer/`) which provides an Ubuntu 22.04 environment with the ARM cross-compiler pre-installed.

**Deploy to hardware:** `build.sh` compiles and deploys to a MiSTer via FTP/SSH. Create a `host` file with the MiSTer's IP address.

There are no unit tests in this project. Testing is done on real MiSTer hardware.

## Architecture

**Entry point:** `main.cpp` — pins to CPU core #1, initializes FPGA I/O, loads storage/user I/O, then enters the main event loop (scheduler-based or fallback polling).

**Key subsystems:**

- **FPGA communication** (`fpga_io.*`) — SPI-based protocol for sending commands/data to the FPGA. All core interaction flows through here.
- **User I/O protocol** (`user_io.*`) — Higher-level protocol layer between the ARM and FPGA cores. Handles core type detection, ROM/file loading, save states, RTC, keyboard/mouse forwarding.
- **Menu/OSD** (`menu.*`, `osd.*`) — On-screen display rendering and the file browser/menu system. `menu.cpp` is the central UI logic.
- **Video** (`video.*`, `scaler.*`) — Video mode management, HDMI/VGA output, scaler filters, HDR, gamma correction.
- **Input** (`input.*`, `joymapping.*`) — Input device discovery, joystick/gamepad mapping, keyboard handling, lightgun support. Up to 6 players.
- **File I/O** (`file_io.*`, `DiskImage.*`) — FAT16/FAT32 access, ZIP/CHD/ISO disk image support, SD card emulation.
- **Configuration** (`cfg.*`) — INI-based config parsing. `MiSTer.ini` in the repo root is the default config with extensive inline documentation of all settings.
- **Scheduler** (`scheduler.*`) — Event-driven main loop scheduling.

**Core-specific support** lives in `support/` subdirectories (e.g., `support/n64/`, `support/psx/`, `support/minimig/` for Amiga). Each contains specialized loading, ROM handling, and hardware emulation logic for that core.

**External libraries** in `lib/`: libco (coroutines), miniz (ZIP), libchdr (CHD format), lzma, zstd, md5, bluetooth, imlib2 (image rendering with freetype/libpng).

## Build System Details

- C files compiled with `-std=gnu99`, C++ with `-std=gnu++14`
- PNG images in the root are linked as binary objects (embedded resources)
- The `main.cpp.o` target depends on all other objects to ensure the build timestamp (`VDATE`) is always current
- Dependency files (`.d`) are auto-generated for incremental builds
