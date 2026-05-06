# early-ramdisk-init

`early-ramdisk-init` is a small early userspace init program used from the ramdisk before the main root filesystem is mounted. It prepares the minimum runtime environment, loads specific kernel modules, runs boot tasklets, mounts the real root filesystem, switches into it, and finally executes the real init process.

The project is written in C, built with autotools, and is designed for Qualcomm-based Linux and virtualized platform bring-up flows where boot ordering and early device availability matter.

## Overview

At a high level, the binary does the following:

1. Mounts temporary runtime filesystems such as `/dev`, `/sys`, and `/proc`.
2. Opens logging and emits boot KPI markers.
3. Parses kernel command line arguments from `/proc/cmdline`.
4. Loads early kernel modules from config files under `/etc/modules-load.f`.
5. Runs registered early tasklets referenced by config.
6. Resolves the real root device, including `PARTUUID=` and `PARTLABEL=` forms.
7. Optionally creates a device-mapper backed root device from `dm-mod.create`.
8. Mounts the real root filesystem at `/realroot`.
9. Optionally layers overlayfs on top of an `erofs` root when dm-verity is not in use.
10. Binds log storage into the real root, switches root, reloads runtime mounts, and runs late tasklets.
11. Executes the final init binary, `/sbin/init` by default.

## Main Features

- Early boot init flow implemented in `src/init/init.c`
- Parallel early module loading with a thread pool in `src/init/fast-modules-load.c`
- Extensible early and late tasklet system using linker sections
- Root device lookup by path, `PARTUUID`, `LABEL`, or `PARTLABEL`
- Device-mapper root setup from kernel cmdline
- Platform-specific module and tasklet configuration under `conf/`
- Boot logging to `/dev/kmsg` and a temporary log filesystem

## Boot Flow

The main control flow lives in `src/init/init.c`.

1. `mount_logfs()` mounts a tmpfs log area.
2. `mount_setup()` mounts `devtmpfs`, `sysfs`, and `proc`.
3. `log_setup()` opens `LOG_DIR/early_ramdisk_init.log`.
4. `rootfs_cmd_setup()` parses `/proc/cmdline` into a `cmd_params` structure.
5. `fast_modules_load()` loads early modules and executes inline early tasklets from `/etc/modules-load.f`.
6. `late_tasklet_init()` preloads late tasklet config files from `/etc/modules-load.l`.
7. If the root device is specified by alias, `rootfs_alias_setup()` resolves it to a block device path.
8. The real root is mounted at `/realroot`.
9. For non-dm `erofs` rootfs, userdata-backed overlayfs is prepared to make the root writable.
10. The log tmpfs is bind-mounted into the real root.
11. The process changes directory to `/realroot`, moves it to `/`, performs `chroot(".")`, and re-enters `/`.
12. Runtime pseudo-filesystems are mounted again in the new root.
13. `late_tasklet_load()` runs deferred late tasklets.
14. `execl()` launches the configured init binary.

## Build Requirements

The autotools files declare the following dependencies:

- C compiler
- `autoconf` / `automake`
- `libkmod`
- `pthread`
- `libblkid`

The binary is linked with a custom linker script, `tasklet.lds`, so tasklet registration sections remain discoverable at runtime.

## Build Instructions

Typical autotools build flow:

```sh
autoreconf -fi
./configure
make
```

The resulting program is:

```text
early-ramdisk-init
```

By default, `configure.ac` sets the installation prefix to `/sbin`.

## Kernel Command Line Parameters

`rootfs_cmd_setup()` in `src/init/init.c` recognizes these parameters:

- `root=`: root filesystem device or alias
- `rootfstype=`: filesystem type for the real root; defaults to `ext4`
- `init=`: init binary to execute after switching root; defaults to `/sbin/init`
- `dm-mod.create=`: device-mapper definition for creating a root device
- `early-ramdisk.mode=`: mode bitmask controlling boot behavior
- `androidboot.slot_suffix=`: slot suffix such as `_a` or `_b`
- `ro`: mount rootfs read-only
- `rw`: mount rootfs read-write

Supported root aliases include:

- `PARTUUID=...`
- `LABEL=...`
- `PARTLABEL=...`

If an alias is used, the code resolves it to a concrete device path before mounting.

## early-ramdisk.mode

`early-ramdisk.mode` is treated as a bitmask.

Current bits used by `src/init/fast-modules-load.c`:

- `0x1`: enable fast parallel early module loading
- `0x2`: allow late tasklets to run without waiting after each forked child

If the value is missing or not greater than zero, the code falls back to a conservative mode.

## Configuration Files

Runtime configuration is consumed from these directories:

- `/etc/modules-load.f`: early module and early tasklet configs
- `/etc/modules-load.l`: late tasklet configs

This repository keeps platform examples under `conf/`, for example:

- `conf/sa8775/01-base.conf`
- `conf/sa8775/tasklet.late`
- `conf/sa81x5/`
- `conf/sa7255/`
- `conf/sa8775-qclinux/`
- `conf/gvm-gen5/`

These files are intended as source configs for packaging or image generation into the runtime locations above.

## Early Config Syntax

Early config files are parsed line by line by `thread_modules_load()` in `src/init/fast-modules-load.c`.

Supported line formats:

- `kernel/.../foo.ko`
  - load a module by path
- `/absolute/path/to/foo.ko`
  - load a module by absolute path
- `:module_name`
  - wait until the named module dependency reaches a usable init state
- `>mask=N`
  - pin the worker thread to CPU `N`
- `@tasklet_name`
  - run a registered early tasklet

Example from `conf/sa8775/01-base.conf`:

```text
kernel/drivers/pinctrl/qcom/pinctrl-msm.ko
kernel/drivers/pinctrl/qcom/pinctrl-sa8775p.ko
kernel/drivers/soc/qcom/qcom-geni-se.ko
@dm_create_tasklet
@wait_rootfs_tasklet
```

## Late Config Syntax

Late config files are simpler. Each valid line starts with `@` and names a registered late tasklet.

Example from `conf/sa8775/tasklet.late`:

```text
@vfio_bind_device_tasklet
@mm_vfio_bind_device_tasklet
@preload_unit_tasklet
@early_mount_tasklet
```

Late tasklets are preloaded by `late_tasklet_init()` and later executed by `late_tasklet_load()` after the root switch.

## Tasklet System

Tasklets are lightweight built-in callbacks registered through macros in `src/include/utils.h`:

- `TASKLET_EARLY_CALL(name, func)`
- `TASKLET_LATE_CALL(name, func)`

The tasklet registry is implemented through linker-defined section boundaries in `src/utils/tasklet.c`. At runtime, config entries such as `@wait_rootfs_tasklet` or `@vfio_bind_device_tasklet` are resolved by name and dispatched to the corresponding function.

Built-in examples include:

- `dm_create_tasklet`: creates a device-mapper root device
- `wait_rootfs_tasklet`: waits for the root block device to appear
- `fetch_socinfo_tasklet`: collects SoC and machine info
- `early_init_tasklet`: executes `/usr/sbin/early_init`
- `vfio_bind_device_tasklet`: runs `/usr/bin/vfio-device-bind.sh`
- `mm_vfio_bind_device_tasklet`: runs `/usr/bin/mm-vfio-device-bind.sh`
- `preload_unit_tasklet`: pre-reads systemd unit files to warm caches
- `early_mount_tasklet`: mounts firmware or DSP related partitions when enabled
- `video_lib_unification_tasklet`: applies platform-specific overlay or bind mounts

Most of these implementations live in `src/utils/tasklet-lib.c`.

## Root Device Resolution

Device lookup logic is implemented in `src/utils/blkid.c`.

The code first attempts a fast path by globbing likely block device names and probing partition metadata directly. If that does not succeed, it falls back to `blkid_get_devname()`. This optimization reduces the time spent resolving the root device during boot.

This mechanism is also used while translating device-mapper table arguments that contain `PARTUUID` values into real device nodes before loading the dm table.

## Logging And Debugging

Logging is implemented in `src/utils/log.c`.

- Kernel-visible logs are written to `/dev/kmsg` with the prefix `early-ramdiskinit: `
- Structured logs are also written to `LOG_DIR/early_ramdisk_init.log`
- Boot KPI markers are emitted through `/sys/kernel/boot_kpi/kpi_values`

Useful places to inspect during bring-up:

- kernel log buffer for early boot progress
- `early_ramdisk_init.log` after the real root is mounted
- module load configs staged into `/etc/modules-load.f`
- late tasklet configs staged into `/etc/modules-load.l`

## Source Tree Overview

- `src/init/init.c`: main init flow, cmdline parsing, root mounting, root switch
- `src/init/fast-modules-load.c`: early module loader and late tasklet config loader
- `src/utils/tasklet.c`: tasklet lookup by name
- `src/utils/tasklet-lib.c`: built-in tasklet implementations
- `src/utils/blkid.c`: root device and partition token resolution
- `src/utils/thread_pool.c`: worker pool for parallel module loading
- `src/utils/log.c`: boot logging helpers
- `src/include/utils.h`: common interfaces, tasklet macros, logging helpers
- `conf/`: platform-specific example configs
- `tasklet.lds`: linker script for tasklet section layout

## Notes

- The code assumes an early userspace environment with enough filesystem layout already present for `/dev`, `/proc`, `/sys`, `/realroot`, and the module config directories.
- Some tasklet behaviors are gated by compile-time macros such as `FIRMWARE_MOUNT` and `VENDOR_DSP_MOUNT`.
- Several platform directories under `conf/` are specific to Qualcomm SoCs and guest/virtual machine variants.

## License

Source files in this project are marked with `BSD-3-Clause-Clear`.
