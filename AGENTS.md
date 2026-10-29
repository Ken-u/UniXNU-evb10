# Agent instructions

This repository owns the EVB10 board integration: memory layout, eMMC root, VOP2 display, touch, Mali
integration, board patches and packaging. Shared RK3588 resources are in `UniXNU-rk3588-common`; the
eMMC and Mali drivers are submodules (`package/Rk3588Emmc`, `package/Mali`). Keep board facts here, SoC
facts in the SoC repository, and driver internals in the driver repositories.

Read these UniXNU pages before editing:

1. [Porting](https://unixnu.com/porting/)
2. [Device repositories](https://unixnu.com/porting/device-repository)
3. [Device trees](https://unixnu.com/porting/device-tree)
4. [Porting records](https://unixnu.com/porting/records)

Rules specific to this repository:

- Only catalog components go into `[components] include`. Board drivers live under `package/` and are
  built by `scripts/build-evb10-drivers.py`. Do not name a script like a UniXNU script: the device
  scripts are copied into the build tree after the UniXNU ones and would replace them.
- The target is Mach-O; inline assembly goes through the Darwin assembler, where `;` starts a comment.
  `scripts/build-evb10-kernel.sh` rejects it in patches.
- Patches apply with plain `git apply` in series order. Run `patches/tools/make-patches.py` against a tree
  that has the ARM64, SoC and OS series applied, never against the device series.
- Do not commit firmware, vendor binaries, generated includes, IPSW or Apple content, logs, images or
  build output. List vendor inputs in `proprietary-files.txt` with their SHA-256.
- Addresses, hostnames, accounts and local paths of the machines that build and run the board belong in
  the ignored `.env`, never in the repository.
- Update `porting.toml` findings or evidence in the same change as the code. Record `device-verified` only
  with a command, a full commit and an accessible log.

Run builds from the UniXNU checkout:

```sh
./unixnu defconfig evb10_defconfig -D ../UniXNU-evb10
./unixnu tree -D ../UniXNU-evb10
python3 scripts/porting-record.py check --device ../UniXNU-evb10
```

Report commands, results, required local inputs, and hardware checkpoints. Write concise English
describing current behavior.
