# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Main_MiSTer is the userspace ARM Linux application that runs on the MiSTer FPGA platform (Intel Cyclone V SoC / DE10-Nano). It bridges the Linux OS and FPGA cores that emulate retro computing platforms, handling core loading, input routing, file/disk image mounting, video configuration, OSD menus, and more.

## Build Commands

Cross-compiled for ARM with `arm-none-linux-gnueabihf-gcc` (GCC 10.2.1). The cross-compiler toolchain must be installed on the build machine.

```bash
make            # Build bin/MiSTer (auto-parallelized via nproc)
make clean      # Remove bin/ directory
make DEBUG=1    # Build with -O0 -g, keeps .elf, no strip
make V=1        # Verbose build output
make PROFILING=1  # Enable profiling instrumentation
```

Output binary: `bin/MiSTer`. There are no tests, linting, or CI targets.

## Architecture

### Communication Model

- **FPGA registers**: Memory-mapped at `0xFF000000` via `/dev/mem` (`fpga_io.cpp`)
- **Shared DDR memory**: Mapped at `0x20000000` for bulk data transfer (`shmem.cpp`)
- **SPI protocol**: Custom protocol with chip-select lines for OSD, core I/O, and file transfer. Commands defined as `UIO_*` constants in `user_io.h` and `spi.h`

### Concurrency

- **Cooperative scheduling** via `libco` coroutines (`scheduler.cpp`): alternates between a "poll" coroutine (user_io_poll, frame_timer, input_poll) and a "UI" coroutine (HandleUI, OsdUpdate)
- **Offload thread**: Separate pthread worker for slow background operations (`offload.cpp`)
- **CPU affinity**: Main process pinned to CPU core 1 (core 0 handles Linux interrupts)

### Main Loop (main.cpp)

`main()` → `fpga_io_init()` → `FindStorage()` → `user_io_init()` → scheduler loop of:
- `user_io_poll()` — SPI communication with FPGA core
- `input_poll()` — evdev input device polling
- `HandleUI()` — OSD menu processing
- `OsdUpdate()` — OSD rendering

### Key Modules

| Module | Purpose |
|--------|---------|
| `user_io.cpp` | Central hub: core detection, SPI protocol, input forwarding, file mounting, video config |
| `fpga_io.cpp` | FPGA register access, bitstream (RBF) loading, FPGA manager state machine |
| `menu.cpp` | OSD menu system, file browser, settings UI |
| `input.cpp` | Linux evdev input: keyboard, mouse, gamepad, lightgun |
| `video.cpp` | Video output: scaler filters, gamma, shadow masks, framebuffer, VRR/FreeSync, HDR |
| `osd.cpp` | On-Screen Display rendering |
| `cfg.cpp` | INI configuration parser (MiSTer.ini) |
| `file_io.cpp` | File I/O abstraction, ZIP support, directory scanning, storage detection |
| `spi.cpp` | SPI communication wrappers |
| `ide.cpp` / `ide_cdrom.cpp` | IDE/ATA/ATAPI emulation for ao486 and other cores |

### Core-Specific Support

`support/<platform>/` directories contain per-core support modules (e.g., `support/psx/`, `support/minimig/`, `support/x86/`). Core type detection happens in `user_io.cpp` via type constants (`CORE_TYPE_8BIT`, etc.) and name-based identification (`is_minimig()`, `is_psx()`, etc.).

### Vendored Libraries (lib/)

`libchdr` (CHD disc images), `libco` (coroutines), `miniz`/`lzma`/`zstd` (compression), `md5`, `sxmlc` (XML parser). Pre-built ARM `.so` files for Imlib2, freetype, libpng, bz2, zlib, bluetooth.

## Code Conventions

- Mixed C (`.c`, gnu99) and C++ (`.cpp`, gnu++14) with C-style idioms throughout
- All source files are flat at repository root (no `src/` directory)
- Functions: predominantly `snake_case`, some `PascalCase` for higher-level functions
- Constants/macros: `UPPER_SNAKE_CASE`
- Types: lowercase with `_t` suffix
- Header guards: `#ifndef FILENAME_H` / `#define FILENAME_H` (no `#pragma once`)
- Heavy use of global state, raw pointers, printf/snprintf, C-style casts
- Linux-specific APIs: `/dev/mem`, evdev, ioctl, `sched_setaffinity`
