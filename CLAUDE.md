# CLAUDE.md

Guidance for working in this repository.

## What this is

This is **MiSTer Main** — the userspace controller binary (`MiSTer`) that runs on
the DE10-Nano's ARM Cortex-A9 under Linux. It drives the FPGA: loading cores,
handling the on-screen menu/OSD, input devices, file/disk I/O, video/scaler
config, and per-core support logic. It is **not** the FPGA cores themselves.

- Entry point: `main.cpp` → `user_io_init()` then the main poll loop
  (`user_io_poll` / `frame_timer` / `input_poll` / `HandleUI` / `OsdUpdate`),
  or the scheduler (`scheduler.cpp`) when `USE_SCHEDULER` is set.
- The process pins itself to CPU core #1 (core #0 handles HW interrupts).

## Target architecture

The binary is a **32-bit ARMv7 hard-float Linux executable**
(`arm-none-linux-gnueabihf`, interpreter `/lib/ld-linux-armhf.so.3`). It must be
cross-compiled — it does not run on the build host (your Mac).

## Building on macOS (Apple Silicon)

ARM's `arm-none-linux-gnueabihf` cross-toolchain has **no macOS build**, so we
cross-compile inside a Linux Docker container. On Apple Silicon the container
runs natively (arm64) using the aarch64-hosted toolchain — full speed, no x86
emulation.

```bash
./build-docker.sh          # cross-compile -> bin/MiSTer
./build-docker.sh clean    # make clean
./build-docker.sh V=1      # verbose build (or pass any make argument)
```

- First run builds the `mister-build` Docker image (`Dockerfile`), which bakes
  the toolchain into `/opt/mister-toolchain` — slow once, then cached.
- The toolchain lives **inside the image**, not the source tree, so `git status`
  stays clean and rebuilds are incremental `make` (a few seconds).
- Verify output: `file bin/MiSTer` should report `ELF 32-bit LSB ... ARM`.

If you ever build on a Linux x86_64 host instead, the native path is
`source setup_default_toolchain.sh && make` (this is what `build.sh` assumes).

## Deploying to a MiSTer

`build.sh` in the repo builds + pushes over FTP + restarts, but it's written for
the Linux host-toolchain flow (uses `plink`/`ftp`). On a Mac, build via Docker
then copy the binary yourself:

```bash
scp bin/MiSTer root@<mister-ip>:/media/fat/MiSTer   # password: 1
# then on the MiSTer: killall MiSTer   (it will relaunch), or reboot
```

## Testing changes

There is no host-runnable test suite — `MiSTer` only does anything meaningful on
real hardware talking to the FPGA. To validate a change:

1. Build: `./build-docker.sh` (must compile clean — warnings are largely
   enabled via `-Wall -Wextra`).
2. Deploy to a real MiSTer (scp above) and exercise the affected feature
   (menu navigation, core load, the specific support module, etc.).

When adding a feature, prefer compiling early and often through Docker to catch
cross-compile issues (the ARM target is stricter about some things than a
host gcc would be).

## Code layout

- `main.cpp` — entry point and fallback main loop.
- `menu.cpp` (~8k lines) — the OSD menu system / UI state machine (`HandleUI`).
- `user_io.cpp` — core communication, core loading, config, the heart of
  host↔FPGA interaction.
- `input.cpp`, `joymapping.cpp`, `input.h` — input devices, gamepad mapping
  (`gamecontroller_db.cpp` is the SDL controller DB).
- `video.cpp`, `scaler.cpp`, `osd.cpp` — video config, scaler, on-screen display.
- `file_io.cpp`, `DiskImage.cpp`, `ide*.cpp`, `cd*` — storage, disk/CD images.
- `fpga_io.cpp`, `spi.cpp`, `hardware.cpp`, `smbus.cpp`, `shmem.cpp` — low-level
  FPGA/hardware/bus access.
- `cfg.cpp` — parses `MiSTer.ini` configuration.
- `support/<system>/` — per-core logic (one dir per system: `snes`, `n64`,
  `megacd`, `psx`, `saturn`, `x86`, `minimig`, `arcade`, etc.). Most new
  per-system features live here.
- `lib/` — vendored libraries (libchdr, miniz, lzma, zstd, libco, bluetooth,
  imlib2, serial_server, md5). Generally don't modify these.

## Conventions

- C++ with a C-ish style; match the surrounding file's idioms (naming,
  brace style, comment density) rather than imposing a new style.
- The Makefile globs sources (`*.cpp`, `support/*/*.cpp`, several `lib/*` dirs),
  so new `.cpp` files in those locations are picked up automatically — no
  Makefile edit needed for a new file in an existing globbed dir.
- Build artifacts go to `bin/` (gitignored, along with `gcc-*`, `host`, `MiSTer`).
