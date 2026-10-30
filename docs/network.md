# Wi-Fi (AP6398S) on the EVB10

Status: **planned**. The module is powered and identified over SDIO, from U-Boot and from the kernel; there is no firmware
download, network interface or Apple80211 glue yet. This page keeps what is known, what was observed on the board, and what
comes next.

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
* U-Boot here has no `sleep` and no `i2c` command; the script waits between ssh round trips, and the RTC was checked with a
  GPIO bit-bang I2C built from `mw.l` macro lines.

## Firmware and board data

Recorded as optional entries in `proprietary-files.txt` (section "AP6398S Wi-Fi", source: the Rockchip Android vendor tree):

| File | What it is |
| --- | --- |
| `fw_bcm4359c0_ag.bin` | Ampak 4359c0 SDIO firmware 9.87.51.11.102 (2023-08-21); the chip is revision 9 |
| `nvram_ap6398s.txt` | `AP6398S_NVRAM_V4.2_20221130`, board type `0x0812`, board revision `0x1301`; it carries a fixed default MAC address |
| `4359_cypress_auto.clm_blob` | regulatory data (CLM); whether the C0 build wants it is not decided |

`fw_bcm4359c51a2_ag.bin` in the same directory is a 4362a2 build, not for this module. Nothing loads these files yet.

## What is missing

1. **A real SDIO host.** `Evb10Wifi` only does CMD52 at 400 kHz, 1-bit. Still to come: 4-bit and a higher clock (the CRU divider
   and the host's CLKDIV, the drive and sample phases in `SDIO_CON0/1` at 0xfd7c0c24/28), CMD53 with block transfers (FIFO or
   the host's internal DMA), and the interrupt (SPI 204, a fixed number: the node must not export `interrupts`). The eMMC host
   (`dwcmshc`, `Rk3588Emmc`) is a different controller.
2. **A Broadcom FullMAC component.** The catalog only has `Ath12kWifi` (Qualcomm, PCIe). The station interface of its
   `nw_abi.h` (`nw_wifi_scan/connect/tx/...`, `nw_wifi_events`) can stay; the board interface changes from PCIe BAR and MSI to
   SDIO function accesses. The reference for the protocol is Linux `brcmfmac` (`sdio.c`, `bcmsdh.c`, `chip.c`): backplane
   windows, firmware and nvram download, SDPCM/BCDC framing, the firmware events.
3. **The OS side.** `WifiNet.cpp` of UniXNU-sheng adapts the network service and the Apple80211 requests; it would be reused
   with the new component. The device needs its own MAC address (the nvram carries a fixed default).
4. **Interrupts.** SDIO in-band interrupt on function 1, and `WL_HOST_WAKE` (GPIO0_B2) for wake-up.

## Next step

Give the in-kernel host CMD53 and 4-bit, then download `fw_bcm4359c0_ag.bin` and the nvram to the chip and see it boot (the
firmware's console and `CHIPID` through the backplane are the checks).
