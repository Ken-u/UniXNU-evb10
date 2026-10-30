# UniXNU EVB10

[UniXNU](https://unixnu.com) device repository for the Rockchip RK3588 EVB10 V10 development board.
It runs the iPadOS 17.7.11 (21H461) userland on a source-built XNU, started from the
[edk2-rk3588](https://github.com/edk2-porting/edk2-rk3588) based firmware of
[`uefi-evb10`](https://github.com/Ken-u/uefi-evb10).

| Repository | Role |
| --- | --- |
| `UniXNU-evb10` (this one) | Board configuration, device tree, board patches, display, input and packaging |
| [`UniXNU-rk3588-common`](https://github.com/Ken-u/UniXNU-rk3588-common) | Shared RK3588 SoC support: CPU, GIC, console, PSCI, eMMC personality |
| [`Rk3588Emmc`](https://github.com/Ken-u/Rk3588Emmc) | eMMC host and block device (`package/Rk3588Emmc` submodule) |
| [`Mali`](https://github.com/Ken-u/Mali) | Mali-G610 kernel driver (`package/Mali` submodule) |
| [`MaliMetal`](https://github.com/Ken-u/MaliMetal) | Metal stack on the Mali: Mesa patches, Metal plug-in, bridge |
| [`uefi-evb10`](https://github.com/Ken-u/uefi-evb10) | Firmware, board tools, serial and reset helpers |

## Contents

| Path | Contents |
| --- | --- |
| `board.toml`, `feeds.toml`, `unixnu.lock` | Board configuration and pinned sources |
| `porting.toml` | Porting record: per-area status, findings and evidence |
| `configs/` | `evb10_defconfig` (no display) and `evb10_display_defconfig` (the image) |
| `dts/evb10.dts` | Board device tree on the shared `rk3588.dtsi` |
| `patches/xnu/` | Board XNU patches and their `series` |
| `patches/tools/` | Sources and generator of the userland-hook patches |
| `package/` | Board package (`kits/evb10` in the build tree): display, input, probes, submodules |
| `scripts/` | Archive builders, storage binding and audit, firmware extraction, kernel build driver |
| `proprietary-files.txt` | Vendor inputs by SHA-256 (Mali CSF firmware) |
| `docs/` | Bring-up notes |

## Build

Build with the [UniXNU CLI](https://unixnu.com/building) (Python 3.11 or newer). Settings of the
machine that builds go into an ignored `.env` (see `.env.example`).

```sh
git clone --recurse-submodules https://github.com/Ken-u/UniXNU-evb10
cd ../UniXNU
./unixnu defconfig evb10_defconfig -D ../UniXNU-evb10
./unixnu fetch -D ../UniXNU-evb10
./unixnu status --json -D ../UniXNU-evb10
./unixnu tree -D ../UniXNU-evb10
python3 scripts/porting-record.py check --device ../UniXNU-evb10

# firmware input, then the two-pass kernel build
../UniXNU-evb10/scripts/extract-files.py <dir holding mali_csffw.bin>
../UniXNU-evb10/scripts/build-evb10-kernel.sh
```

`./unixnu build payload -D ../UniXNU-evb10 --unique-chip-id <hex>` needs a platform seed that is not
stored in any repository. Firmware, boot flow and the serial and reset tooling are in `uefi-evb10`.

## Status

The format 2 configuration materialises, resolves and passes the porting-record check. The full
XNU series (ARM64, SoC, OS and device patches) applies to a fresh XNU clone, and
`scripts/build-evb10-kernel.sh` builds both kernel passes on Linux (kernel sha256
`db5e4c058e6516d9cb886d2819d1ca02366a29b9748fbd6b15123e0023e5bf2f`). The results recorded in
`porting.toml` were first observed with an earlier repository layout. Since then every piece has been rebuilt
from the pushed repositories in an empty directory ([`docs/reproduce.md`](docs/reproduce.md)) and booted on the board
(2026-10-29 and 2026-10-30): eMMC root and Data, eight cores (five with the frame-rate defaults), `launchd` with about
110 processes, SpringBoard and backboardd, Mali-composited frames (`vkpipe failed=0`) and no panic over captures of 130 to
235 s. The panel and the touch screen were checked by hand on that system and work normally. All areas in scope are
therefore `device-verified`; the excerpts of the serial captures behind each entry are in [`evidence/`](evidence/).

Scrolling the home screen gets about 50 frames per second in the steady state with the default `kernel_args`, which carry
the frame-rate group: zero-copy scanout with the GPU fence, the GPU at 750 MHz, the device's YCbCr filter, and the render
server's encoding thread kept off the Cortex-A55 cores (52.6 frames per second on average over scripted swipes 3 to 11 of a
default boot, 57.5 at best; MaliMetal's `docs/notes.zh-CN.md`, "帧率" and "帧率（二）"). At 750 MHz 8.7 % of the heavy command
buffers miss their 19.3 ms deadline; a 1 GHz GPU (firmware built with `GPU_RAIL_UV=850000` plus `mali-npll=1000`, see
[`docs/reproduce.md`](docs/reproduce.md)) brings that to 1.6 to 2.5 %. Open items: a right swipe starts about 100 ms after
a left one (cause not proven), fixed GPU and CPU clocks, interrupt-driven GPU completion, a finger on the touch controller at
400 kHz (the default 100 kHz was checked by hand), network, audio and power management.
