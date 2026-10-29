# Reproducing the build from the six repositories

Checked on 2026-10-29 by building every piece in an empty directory from the pushed repositories only (no files from the
earlier scratch trees) and booting the result on the board: eMMC root and Data mounted, `launchd` with ~110 processes,
`watchdogd` exits 0, SpringBoard and backboardd run, Mali composites (`vkpipe failed=0`), no panic in a 130 s capture.

## Layout

```
ws/
  UniXNU/                  upstream UniXNU (+ scripts/host/unixnu-linux-rootless.patch when you have no root)
  UniXNU-rk3588-common/    SoC repo         (feeds.toml pins it; local.toml points at the checkout while developing)
  UniXNU-evb10/            this repository  (package/Mali, package/Rk3588Emmc are submodules)
  uefi-evb10/              firmware + board tools  (needs edk2-rk3588 at the commit in its README)
  MaliMetal/               PanVK / Metal stack     (+ metal2vulkan, mesa-build outputs next to it)
```

## Order

1. Kernel, two passes: `scripts/build-evb10-kernel.sh` (pass A `out/evb10/<os>/evb10`, pass B display archive
   `.../evb10_display`). `package/Mali/gpu_files.inc` is generated and linked in, so the kernel has to be rebuilt after
   step 3.
2. UEFI: `uefi-evb10/build.sh`. Scrub `PATH`/Python variables that point at another toolchain first (pyelftools).
3. GPU user stack, in this order: `MaliMetal/mesa-build/*` (LLVM, SPIRV, libclc, libdrm, host and iOS Mesa) ->
   metal2vulkan + `metal-plugin/m2v-patches/apply.sh` -> `metal-plugin/build-bridge.sh` -> `usertest/build.sh` ->
   `metal-plugin/build-plugin.sh` -> `usertest/build.sh` again (picks up the plugin cdhash) -> rebuild the kernel.
4. `uefi-evb10/tools/xnu-probe/run-kernel.sh TAG` repacks the payload (`make-payload-boot.sh`) and boots the board.

## Things that bite

* `usertest/build.sh` rewrites the second `trust_cache_sha256` entry in `board.toml`. The userspace binaries are not
  bit-for-bit reproducible, so the committed value belongs to one particular build and changes after every rebuild.
* `board.toml [adt.defaults.u32]` must keep `sleep-disabled` and `vmm-present`. `vmm-present` backs
  `kern.hv_vmm_present`; without it `watchdogd` traps three times and the kernel panics ("watchdogd exited").
  Only `kern.maxvnodes` comes from the OS profile.
* Do not give an exported DTS node an `interrupts` property (see the note at the top of the SoC `rk3588.dtsi`): the ADT
  node would get `interrupt-parent` but XNU resolves parents through `AAPL,phandle`, and IOKit start-up takes a data abort.
* Submodule pointers (`package/Mali`, `package/Rk3588Emmc`) must be staged (`git add package/Mali ...`) or `unixnu`
  resets them to the index commit.
* `.env` of `uefi-evb10` needs `ADB`, `FB`, `MTOOLS_BIN` and the host/board addresses (see `.env.example`).

## Without root (`scripts/host/`)

`env.sh` (set `TOOLS` to a prebuilt UniXNU toolchain prefix, `WS` to the workspace) and `in-ns.sh` (user namespace that
overlays the `xcrun`/`plutil` shims on `/usr/bin`) run the upstream build on a machine where you cannot install packages.
`unixnu-linux-rootless.patch` is the small patch against upstream UniXNU that goes with it (git older than 2.27,
`strlcpy` from libbsd, `package_manager none`). The firmware build needs the `uuid` development headers: unpack the
`uuid-dev` deb into a prefix and point `UUID_PREFIX` at it.
