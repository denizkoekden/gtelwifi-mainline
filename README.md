# Mainline Linux for the Samsung Galaxy Tab E 9.6 (SM-T560, "gtelwifi")

> **Developed with AI assistance.** The kernel patches, tools and packages in this repository were written with
> the help of an AI coding assistant (Claude, by Anthropic). Every kernel patch says so in an `Assisted-by:` tag, as
> the kernel's `Documentation/process/coding-assistants.rst` asks. Everything was built and tested on real hardware,
> but please keep this in mind when you review, reuse or submit any of it to a project: check that project's policy
> on AI-assisted contributions first.

Linux 7.3.1 with 79 patches on the Spreadtrum/Unisoc SC7730SE (4x Cortex-A7 at up to 1.3 GHz, Mali-400 MP2, 1.5 GB
RAM, PMIC SC2723), running a postmarketOS-based userspace with Phosh. The international SM-T560 only; the SM-T560NU
has a Snapdragon 410 and needs a different port.

## What works

- SMP on all four cores with the ARM architected timer, cpufreq 768–1300 MHz (optional steps up to 1612 MHz),
  thermal throttling
- eMMC (DDR52), microSD, USB gadget (network and serial console)
- touchscreen, capacitive keys, all hardware keys (also as wake-up sources)
- charging (SMB358 with the SM5504 MUIC selecting the input current), fuel gauge, battery temperature
- WLAN and Bluetooth (BCM43430A1), the factory MAC/BT addresses read from the device at run time
- backlight, accelerometer, RTC, suspend (secondary cores off, CPU0 in WFI, woken by RTC and keys)
- a DRM driver for the display controller (takes over the pipeline the bootloader leaves running) and GPU
  acceleration with lima
- speaker audio
- both cameras through V4L2, libcamera (with a small pipeline patch) and PipeWire

Not yet: DSI/panel power-down and deep sleep, headphones and microphones, camera still capture and autofocus,
GPS (needs a proprietary host-side engine).

## Layout

| Path | Content |
|---|---|
| `patches/` | the kernel patches against Linux v7.3.1 stable (`git am`) |
| `buildkit/` | kernel config fragments, boot image packer (`mkimage.py`, SPRD `dt.img`), rescue initramfs, test programs |
| `docker/` | build container |
| `tools/` | flashing, rescue-system access, test and analysis helpers |
| `packages/` | APKBUILDs for a postmarketOS-based distribution: kernel, device package, Wi-Fi NVRAM |

## Build

`docker/run.sh` builds the container on first use and keeps the kernel tree in a Docker volume (macOS file systems
are case-insensitive). Put your SSH public key into `buildkit/ssh/authorized_keys` first (see `buildkit/ssh/README`),
then:

```bash
docker/run.sh sh -c "git clone --depth 1 -b v7.3.1 https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git /src/linux \
	&& cd /src/linux && git -c user.name=build -c user.email=build@localhost am /work/patches/*.patch"
docker/run.sh env O=/src/kout LLVM=1 EXTRA_CONFIG=/work/buildkit/cfi-enforce.config ./buildkit/build.sh /src/linux
```

The result is `buildkit/gtelwifi-mainline.img`: an Android v0 boot image with a small rescue initramfs (status screen,
USB network `10.55.0.1`, SSH with your key, serial shell over USB). Without `LLVM=1` and `EXTRA_CONFIG` it builds
with GCC and without kCFI.

## Flashing — read this first

- **Only ever write the `KERNEL` partition** (`mmcblk0p20`, 16 MiB). Do not touch the bootloader, PARAM or the
  partition table: as long as they are intact, Download mode (Volume Down + Home + Power) always works.
- Make a full backup first (TWRP), including `efs`, which holds the factory network addresses.
- The bootloader does not check signatures. From TWRP: *Install → Install Image → Boot*. From a running rescue
  system: `tools/tabflash <image>`.
- Emergency exit: hold Power + Volume Down for about 10 seconds.

## Proprietary parts, not included

- **Camera sensor register tables:** the sensors need register tables that only exist in the stock Android camera
  HAL. `tools/camtables.py --firmware` extracts them from your own TWRP system backup into files for
  `/lib/firmware/{sr200pc20m,s5k4ecgx}/`. Do not redistribute the output.
- **Wi-Fi:** firmware from linux-firmware; the board NVRAM is fetched from a public device tree repository by
  `packages/firmware-samsung-gtelwifi`.
- **Bluetooth:** the BCM43430A1 patch RAM from linux-firmware works.

## License

GPL-2.0-only (`LICENSE`), like the kernel the patches are for. Files with their own SPDX header keep that license.
