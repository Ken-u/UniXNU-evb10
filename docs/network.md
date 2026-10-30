# Wi-Fi (AP6398S) on the EVB10

Status: **planned**. The module is powered, identified over SDIO and its firmware is downloaded and started by the kernel
(HT clock available, function 2 ready); there is no protocol layer (SDPCM/BCDC), network interface or Apple80211 glue yet.
This page keeps what is known, what was observed on the board, and what comes next.

## Hardware

The board carries an Ampak AP6398S, a Broadcom BCM4359 Wi-Fi 5 / Bluetooth combo module.

| Item | Value | Source |
| --- | --- | --- |
| Wi-Fi bus | SDIO 3.0 on `mmc@fe2d0000` (Synopsys DW-MSHC v2.70a), 4-bit, SDR104-capable, SPI 204 | stock device tree; the controller version was read on the board |
| Pins | `sdiom0`: GPIO2_A6/A7 = D0/D1, GPIO2_B0/B1 = D2/D3, GPIO2_B2 = CMD, GPIO2_B3 = CLK, function 2 | stock device tree |
| Power | `WL_REG_ON` = GPIO0_C4 (high = on); the module's 32.768 kHz low-power clock is the CLKOUT of the HYM8563 RTC (I2C6, address 0x51) | stock device tree (`mmc-pwrseq-simple`, post-power-on delay 200 ms) |
| Wake | `WL_HOST_WAKE` = GPIO0_B2 | stock device tree |
| Bluetooth | UART9 (not part of this page) | stock device tree |
| Clock tree | `cclk_src_sdio` = `CLKSEL_CON(172)` (0xfd7c05b0) bits 9:8 source, 7:2 divider; gates in `CLKGATE_CON(75)` | Linux `clk-rk3588.c` |

## Checkpoint 1: power up and read the chip identity (done)

`uefi-evb10/tools/board/sdio-probe.py` does it from U-Boot, with no kernel (`--enter` takes the board to the U-Boot prompt
first; reset it with `tools/board/hw-reset.sh reset` for a clean state):

```
cd uefi-evb10 && tools/board/hw-reset.sh reset; sleep 25; python3 tools/board/sdio-probe.py --enter --log LOG
```

Observed on the board (log excerpt: `evidence/wifi-sdio-id-20261030.txt`):

* the card answers CMD5 with OCR `0xb0ffff00` (ready, 3 IO functions, 2.0 to 3.6 V) and CMD3 with RCA 1;
* CCCR revision byte `0x43`, CIS at `0x10ac`, `CISTPL_MANFID` = `02d0:4359` (Broadcom, BCM4359);
* with function 1 enabled and the ALP clock requested (`CHIPCLKCSR` reads `0x48`), the backplane window pointed at
  ChipCommon (0x18000000) reads `CHIPID` = `0x17294359`: chip `0x4359`, revision 9, which is the C0 stepping;
* all of it at 400 kHz, 1-bit, from the 24 MHz oscillator.

## Checkpoint 2: the kernel reads the same identity (done)

`package/Evb10Wifi/Evb10Wifi.cpp` is the probe above as kernel code. The display probe starts it from a thread call when the
boot argument `evb10-wifi=1` is given (not in the defaults of `board.toml`: add it per run, for example
`EXTRA_KERNEL_ARGS="evb10-wifi=1"` with `run-kernel.sh`). It maps the host, GPIO0, GPIO2, BUS_IOC and the CRU, opens the SDIO
gates, sets the pins, power-cycles WL_REG_ON (100 ms low, 300 ms high), runs the polled DW-MSHC at 400 kHz in 1-bit mode and does
CMD0, CMD5, CMD3, CMD7 and the CMD52 reads of CCCR, CIS and ChipCommon. Its archive (`libEvb10Wifi.a`) is linked by the display
kernel only, because the display probe is its only caller.

Observed on the board (`evidence/wifi-kernel-id-20261030.txt`): the same answers as from U-Boot (OCR `ffff00`, 3 IO functions,
RCA 1, `MANFID 02d0:4359`, `CHIPID 17294359`, `CHIPCLKCSR 48`), 0.43 s after the driver starts, and the system runs on to
SpringBoard as before. At kernel entry `CLKSEL_CON(172)` reads `0x0c` (GPLL, divider 3) and both SDIO gates and resets are open, so
the driver sets the mux to the 24 MHz oscillator itself.

## Checkpoint 3: the kernel downloads the firmware and the chip answers (done)

With `evb10-wifi=2` the driver goes on after the identification (`evb10-wifi=1` stops there). The sequence was first run by hand
at the U-Boot prompt (registers written with `mw.l`, FIFO words read with `md.l`), then written as kernel code:

1. **Bus.** CCCR `0x07` = `0x82` (4-bit, card-detect pull-up off), the host's `CTYPE` = 1, `cclk_src_sdio` from GPLL with divider
   field 23 (`CLKSEL_CON(172)` = `0x5c`: 1188 MHz / 24 = 49.5 MHz) and the host's `CLKDIV` = 0, which gives a card clock of about
   24.75 MHz. Function 1's block size (registers `0x110/0x111` of function 0) is 64.
2. **CMD53.** Polled: the words move through the host's FIFO (`0x200` answers on every word of `0x200..0xfff`). Byte mode below
   64 bytes (registers), 64-byte blocks from 64 bytes up; the count in the argument is bytes below the block size, blocks above.
   A byte-mode count above the block size answers R5 `OUT_OF_RANGE` and leaves the FIFO full: reset it (`CTRL` bit 1) before every
   transfer.
3. **Backplane.** 32 KiB windows of function 1 (`0x1000a..0x1000c` hold the window base), 32-bit access flag `0x8000` in the
   address. The cores of this chip (read from the EROM at `0x1810c000`, `ChipCommon + 0xfc`): ChipCommon `0x18000000`, 802.11 at
   `0x18001000` and `0x18004000`, ARM CR4 `0x18002000` (wrapper `0x18102000`), SDIO device `0x18005000`.
4. **Passive.** CR4 reset with the halt bit (the AXI wrapper registers `IOCTL` at `+0x408`, `RESET_CTL` at `+0x800`), both 802.11
   cores disabled (`brcmf_chip_set_passive`). The CR4's bank registers give 7 banks of 128 KB: 0xe0000 bytes
   of RAM at `0x160000` (`brcmf_chip_tcm_ramsize`; rambase of revision 9 and later).
5. **Download.** The firmware at the RAM base, the nvram blob (processed like `brcmf_fw_nvram`: comments and blank lines dropped,
   `key=value\0`, a length token at the end) at the top of the RAM; both are read back and compared. The first word of the
   firmware is the reset vector and goes to address 0.
6. **Start.** SDIO core interrupt status cleared, CR4 released from halt, HT clock requested (`CHIPCLKCSR` bit 4), the SDIO core's
   mailbox word set to protocol version 4, function 2 enabled (`IOEx` = 6).

Observed on the board (`evidence/wifi-firmware-run-20261030.txt`): the images (673934 + 4896 bytes) are written in 90 ms and read
back identical in 82 ms; after the release the HT clock is available in 34 ms (`CHIPCLKCSR` `0xd0`) and function 2 reports ready
(`IORx` = 6) 232 ms after the release, which is how `brcmfmac` decides that the firmware started. The system runs on as before.
The images are embedded in the kernel by `scripts/build-wifi-firmware.py` (a `board.toml` archive step; the generated
`package/Evb10Wifi/wifi_fw.inc` is git-ignored) from `.cache/firmware`, which `scripts/extract-files.py` fills from the vendor tree;
without the files the driver prints that nothing is embedded and stops after the bus switch.

## Findings from the bring-up

* **The IOMUX registers are in BUS_IOC, not in the tables `pinctrl-rockchip.c` shows first.** For GPIO2_A4..B7 the function
  select is `GPIO2A_IOMUX_SEL_H` (0xfd5f8044) and `GPIO2B_IOMUX_SEL_L` (0xfd5f8048). The offsets of `rk3588_ds_regs` and
  `rk3588_p_regs` look the same, but they sit in other IOC blocks and are drive strength and pull. Writing them as function
  selects leaves the CLK pad stuck low, and the card never answers (CMD5 times out). Check the pad: with CLKENA set, GPIO2
  `EXT_PORT` (0xfec30070) bit 11 must flip between reads.
* The HYM8563 CLKOUT is on after reset (register 0x0d reads `0x80`), so U-Boot needs no setup for the low-power clock. The kernel
  must not turn it off.
* The first response read after a `mw.l` of the command register can still be the previous command's: a command takes over 100
  microseconds at 400 kHz and U-Boot's `md.l` follows within a few. The script reads RINTSTS three times before RESP0.
* **Byte-mode CMD53 stops at the block size.** Reading 256 bytes in byte mode with function 1's block size at its default came back as
  R5 `OUT_OF_RANGE` and a data read timeout with a stale FIFO (424 words). Set the block size (64) and use block mode.
* The processed nvram and the firmware's own first word are all the download needs: the chip came up with the default drive and
  sample phases of `SDIO_CON0/1` (reset values) at about 25 MHz.
* U-Boot here has no `sleep` and no `i2c` command; the script waits between ssh round trips, and the RTC was checked with a
  GPIO bit-bang I2C built from `mw.l` macro lines.

## Firmware and board data

Recorded as optional entries in `proprietary-files.txt` (section "AP6398S Wi-Fi", source: the Rockchip Android vendor tree):

| File | What it is |
| --- | --- |
| `fw_bcm4359c0_ag.bin` | Ampak 4359c0 SDIO firmware 9.87.51.11.102 (2023-08-21); the chip is revision 9 |
| `nvram_ap6398s.txt` | `AP6398S_NVRAM_V4.2_20221130`, board type `0x0812`, board revision `0x1301`; it carries a fixed default MAC address |
| `4359_cypress_auto.clm_blob` | regulatory data (CLM); whether the C0 build wants it is not decided |

`fw_bcm4359c51a2_ag.bin` in the same directory is a 4362a2 build, not for this module. The kernel embeds the first two (checkpoint
3); the CLM blob is not loaded: the firmware starts without it, and it is only needed once regulatory settings matter.

## What is missing

1. **A fuller SDIO host.** `Evb10Wifi` runs 4-bit at about 25 MHz with polled CMD53 through the FIFO, one-shot at start-up. Still to
   come: the sample and drive phases in `SDIO_CON0/1` (0xfd7c0c24/28) and 50 MHz or more, the host's internal DMA for the
   frames, and the interrupt (SPI 204, a fixed number: the node must not export `interrupts`; function 1's in-band interrupt).
   The eMMC host (`dwcmshc`, `Rk3588Emmc`) is a different controller.
2. **A Broadcom FullMAC component.** The catalog only has `Ath12kWifi` (Qualcomm, PCIe). The station interface of its
   `nw_abi.h` (`nw_wifi_scan/connect/tx/...`, `nw_wifi_events`) can stay; the board interface changes from PCIe BAR and MSI to
   SDIO function accesses. The reference for the protocol is Linux `brcmfmac` (`sdio.c`, `bcmsdh.c`, `chip.c`): backplane
   windows, firmware and nvram download, SDPCM/BCDC framing, the firmware events.
3. **The OS side.** `WifiNet.cpp` of UniXNU-sheng adapts the network service and the Apple80211 requests; it would be reused
   with the new component. The device needs its own MAC address (the nvram carries a fixed default).
4. **Interrupts.** SDIO in-band interrupt on function 1, and `WL_HOST_WAKE` (GPIO0_B2) for wake-up.

## Next step

Talk to the running firmware: the SDPCM frame layer on function 2 (the mailbox and the interrupt status of the SDIO core, the
frame header with sequence and channel), then BCDC iovars and ioctls (`cur_etheraddr`, `ver`, scan) over it, as `brcmfmac` does.
The firmware's console in RAM (the last word of the RAM points to the shared structure that holds its address) is the way to
see why it refuses something. That is the start of the Broadcom FullMAC component of the previous section.
