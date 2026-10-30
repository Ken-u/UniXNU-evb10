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
* `.env` of `uefi-evb10` needs `ADB`, `FB`, `MTOOLS_BIN` and the host/board addresses (see `.env.example`). Its run scripts
  also use the board tools of the SoC repo (`UniXNU-rk3588-common/tools/board`: serial capture, U-Boot console, USB
  helpers, reset wrapper); the checkout next to `uefi-evb10` is found automatically, `RK3588_COMMON` overrides it.

## Frame rate: the default boot arguments, measuring, and a 1 GHz GPU

`board.toml` carries the frame-rate boot arguments (the comment above `kernel_args` says what each one does): zero-copy scanout with the GPU job as
the fence, the GPU at 750 MHz (`mali-pll=1 mali-div=1`: CPLL 1500 MHz divided by two, at the 0.75 V the firmware sets), the device's own YCbCr filter,
the asynchronous surface flush, `cpumask=31` (the four Cortex-A76 cores, which the SoC device tree lists first, and the boot core), and the encoding
thread's priority with the slow-core avoidance (`mali-epri=50`, `sched-slowcpus=240`). `evb10-ps` and the `evb10-bt*` names are not in the list: the
process table costs the serial line about 3 MB a run and the frame rate its measurements. `unixnu` refuses a kernel command line longer than
**1000 bytes** (the defaults are 884), so extra arguments have to be few; arguments of the same name replace the default.

Measure a run (the board runs the scripted swipes of `evb10-touch-demo` and the plug-in records the command-buffer timeline):

```
cd uefi-evb10
EXTRA_KERNEL_ARGS="evb10-touch-demo=120 mali-timeline=1" CAPTURE_SECS=235 EARLY_STOP=0 \
  setsid nohup tools/xnu-probe/run-kernel.sh TAG > runs/TAG.driver.log 2>&1 </dev/null &
python3 tools/xnu-probe/frame_phases.py TAG            # frames per second per gesture, finger down and settling
python3 tools/xnu-probe/cb_table.py TAG --latency      # commit-to-completion percentiles, replays on a slow core, touch-to-first-frame
```

**GPU at 1 GHz.** The clock needs the GPU rail at 0.85 V (Rockchip's operating points: 0.75 V up to 800 MHz, 0.80 V for 900 MHz, 0.85 V for 1 GHz), and
the driver does not scale the voltage, so the two have to be changed together:

1. Build the firmware with the rail in microvolts: `cd uefi-evb10 && env -i HOME=$HOME USER=$USER PATH=/usr/local/bin:/usr/bin:/bin GPU_RAIL_UV=850000 ./build.sh`,
   then `tools/stub/build.sh` (`run-kernel.sh` boots a probe image that carries the firmware volume). A firmware that is flashed into the board keeps its own
   rail: build it the same way and flash it before you run the GPU at 1 GHz from flash.
2. Replace `"mali-pll=1", "mali-div=1"` in the `board.toml` list by `"mali-pll=3", "mali-div=0", "mali-npll=1000"`, or try it for one run with
   `EXTRA_KERNEL_ARGS="mali-pll=3 mali-div=0 mali-npll=1000" tools/xnu-probe/run-kernel.sh TAG`. `mali-pll=3` is NPLL, which the firmware leaves at 850 MHz;
   `mali-npll=MHz` (400 to 1100) moves it before the GPU is switched onto it.
3. The console shows `mali: mali-npll=1000: m 425 -> 500, npll now 1000000 kHz (locked 1, ...)`, and the GPU cycle-counter probe reads 1000 MHz.

1 GHz on the 0.75 V rail is outside the vendor's operating points and was not tried. Numbers for both settings are in the porting record (area `gpu`) and in the MaliMetal notes.

## Without root (`scripts/host/`)

`env.sh` (set `TOOLS` to a prebuilt UniXNU toolchain prefix, `WS` to the workspace) and `in-ns.sh` (user namespace that
overlays the `xcrun`/`plutil` shims on `/usr/bin`) run the upstream build on a machine where you cannot install packages.
`unixnu-linux-rootless.patch` is the small patch against upstream UniXNU that goes with it (git older than 2.27,
`strlcpy` from libbsd, `package_manager none`). The firmware build needs the `uuid` development headers: unpack the
`uuid-dev` deb into a prefix and point `UUID_PREFIX` at it.
