# Wi-Fi (AP6398S) on the EVB10

Status: **implemented** (opt-in with a boot argument). The kernel brings the module up, downloads its firmware, scans, joins a WPA2-PSK
network with the firmware's supplicant, and serves it to iPadOS as `en0`: the OS's own DHCP client gets a lease, DNS resolves and TCP
carries traffic (about 100 MB in one 240 s run). What is observed and what is not is in the checkpoints below. It is off in the committed
boot arguments: `evb10-wifi=N` is added per run, and the network to join comes from a local file that is not in the repository.
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

## Checkpoint 4: the firmware answers and a scan lists access points (done)

`evb10-wifi=3`. After the download the driver speaks SDPCM on function 2 (a 4-byte hardware header with the length and its complement,
a software header with sequence, channel and data offset; flow control and the credit window come back in the receive headers) and
BCDC on top of it (ioctls on channel 0, events on channel 1, data on channel 2). It reads the version, the MAC address and the CLM
version with iovars, sets `bus:txglom` to 0 (no superframes from the chip) and `mpc` to 0, programs `event_msgs`, sends `WLC_UP`,
and runs an `escan` three times.

Observed on the board (`evidence/wifi-scan-20261030.txt`, run `wifi5`): `wl0: Aug 21 2023 11:05:11 version 9.87.51.11.102`, CLM API 15.0,
scan version 1; the scans take 1.5 to 1.7 s and list 35, 38 and 43 networks on both bands (2.4 GHz 20 and 5 GHz 15 in the first list).

## Checkpoint 5: a WPA2-PSK join and a data path of the driver's own (done)

`evb10-wifi=4`. The credentials come from `.cache/firmware/join.txt` (SSID on the first line, passphrase on the second; local, git-ignored,
embedded into the kernel by `scripts/build-wifi-firmware.py`). The join is the firmware's: `WLC_SET_INFRA` 1 and `WLC_SET_AP` 0 first,
then `bsscfg:wpa_auth` 0x80 (WPA2-PSK), `bsscfg:auth` 0, `bsscfg:wsec` 4 (AES), `sup_wpa` 1, the passphrase through `WLC_SET_WSEC_PMK`, and
`bsscfg:join` (`ext_join`, 68 bytes). The events 3 (auth), 16 (link), 7 (assoc) and 46 (`PSK_SUP`, status 6 = keyed) mean the firmware's
4-way handshake finished. The driver then builds DHCP, ARP and ICMP echo frames itself and sends them through the same
transmit path that `nw_wifi_tx` uses (BCDC data header `{0x20, 0, 0, 0}` plus the Ethernet frame on channel 2).

Observed on the board (`evidence/wifi-join-20261030.txt`, run `wifi9b`): the join takes 2.1 s, DHCP binds 3.1 s later, the router's MAC address
is resolved and 4 of 4 pings are answered; 11 frames sent, none failed or dropped.

## Checkpoint 6: iPadOS uses the interface (done)

`evb10-wifi=5` is the same without the self-test, and `package/Evb10Wifi/WifiNet.cpp` (the network side, adapted from the sheng port and the
`nw_abi.h` interface of Ath12kWifi) serves it to the OS:

* `en0` is an `ifnet` made with `ifnet_allocate_extended` (`IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO`: without the second flag the stack
  puts a compatibility nexus in front of a Wi-Fi `en` interface and input no longer reaches the stack the way a legacy driver's does),
  Ethernet framing, `bpf_attach` (the DHCP client sends through it while the interface has no address), the module's own MAC address.
* The registry entries the userland looks for (`IONetworkStack`, `IO80211Controller`, `IO80211Interface`) are classes registered late: the
  object has its static initializer removed (`scripts/build-evb10-wifinet.py`) and the driver runs it between `OSMetaClass::preModLoad`
  and `postModLoad`, once the interface exists.
* The Apple80211 requests of the userland (state, SSID, BSSID, channel, RSSI, scan results, country, ...) are answered from the station state
  and the BSS list; link-up and link-down are posted as kernel events.

Observed on the board (`evidence/wifi-os-20261030.txt`, run `wifi19`, 200 s, no panic): the join takes 2.2 s, IOKit is quiet 26 s into the boot
log, the userland's own DHCP client has its address by the 83 s status line (no DHCP packet at 73 s), and DNS and 36 outgoing TCP connections
follow. An earlier 240 s run (`wifi17`) received 105 MB of TCP payload through `en0` (the best 10 s interval 34 Mbit/s) with the link up
throughout. The packets are the userland's; the kernel only carries them. Nobody looked at the screen or at Settings.

**Side effects.** Once the system has a network it behaves like a device that has one: it downloads assets in the background
(`mobileassetd`, `nsurlsessiond`) and the 2 GiB Data window (about 250 MB free after a normal boot) can fill up within a few minutes,
after which those daemons see `ENOSPC`; the next boot had its free space back. Nothing else is affected, which is why Wi-Fi stays opt-in.

## Checkpoint 7: the Settings Wi-Fi pane (done: list, join with a password, status, forget; 5 GHz is not)

The pane talks to `wifid`, not to the driver, and `wifid` is a launchd job (`com.apple.wifid.plist` in the root image) with
`_LimitLoadToDeviceTree = ["wlan", "marconi-wifi"]`: it starts only on a device whose Apple device tree has a node named `wlan`. The EVB10's
tree had none, so the daemon was never launched and nothing in the OS asked `en0` anything (run `set2`: no `wifid` process). `dts/evb10.dts` now
has a `wlan` node (`unixnu,adt-name`, no registers, no interrupts: the driver addresses the module by its fixed physical base). The node is in
every boot; without `evb10-wifi=N` the daemon finds no `en0`.

Observed on the board (`evidence/wifi-settings-20261031.txt`, runs `set5` and `set9`, `evb10-wifi=5`, no panic): `wifid` is in the process table
from about 75 s and sends Apple80211 requests to `en0` (state, SSID, BSSID, card capabilities, current network, supported channels, country,
radio info, a property list per state change). What `WifiNet.cpp` changed because of them:

* **Current network (103)** answered `ENOENT` 132 times while the link was up, because the join at boot never scanned and the associated
  access point was not in the BSS list; it is now described from the link's own state (SSID, BSSID, channel, RSSI).
* **Type 383 (4104 bytes), 470 and 471 (one byte)** are sets, sent at every state change: 383 carries a property list of the interface
  (`IO80211InterfaceIPConfigurationBusy`, `IO80211InterfaceIPV4Params`, `IO80211InterfaceWifidEnumeratedInfra`). They are accepted and ignored;
  refusing them made the daemon repeat them (34 times in 200 s). The log prints the first three lists.
* Still refused (`EOPNOTSUPP`, the daemon copes): 6, 18, 25, 57, 96, 97, 106, 207 (the channels-info structure, 10016 bytes, format not known),
  387, 419, 423, 473.

**The pane, driven without a person.** `evb10-touch-script=SECS` (`Evb10Touch.cpp`) plays swipes and taps from `package/Evb10Touch/touch_script.inc`
(git-ignored, compiled in). `scripts/gen-wifi-touch-script.py` writes it from the join file: the passphrase becomes key positions of the on-screen
keyboard and is never printed (`info`, `forget` and `rejoin` extend the script with the network's page, Forget This Network and a second join).
With `evb10-ps=12 evb10-shot-img=1 evb10-shot-scale=2` the serial log carries a screenshot every 28 s (`uefi-evb10/tools/xnu-probe/shot2png.py`
turns them into PNGs), and `evb10-wifi-join=2` leaves the join file alone at boot and lists only its network in scans, so the network is the first row
of the list. The script, whose first step plays at about 127 s: two swipes up (the first one is always lost), Settings in the Dock, the Wi-Fi row,
the network's row, the passphrase key by key, Join.

Observed on the board (`evidence/wifi-settings-pane-20261031.txt`, runs t36 to t43; the screenshots are described, not stored, because a script that
missed a row once typed the passphrase into a visible field):

* Settings opens; its Wi-Fi row reads "Not Connected"; the Wi-Fi pane lists the network with a lock and the signal glyph; a tap on it opens
  "Enter the password for ...".
* After the typed passphrase and Join (runs t37, t42, t43): the association request (type 20, cipher 6, the 32-byte key the daemon derived) reaches
  `WifiNet.cpp`, the firmware joins 1.7 s later (events 3, 16, 7), DHCP binds 3 s after that, and **the interface's busy flag
  (`IO80211InterfaceIPConfigurationBusy`) goes false 4.5 to 9.5 s after the link**, which is when the row's spinner turns into the check mark. The status
  bar shows the Wi-Fi glyph and the real time (the system fetched it over the network), the network's page shows the address, mask and router from
  DHCP, and the system opens TCP connections to Apple servers (92 KB received 13 s after the join in t37).
* Forget This Network and its confirmation (run t43): the daemon asks for a disassociate 0.6 s after the confirmation tap, the link goes down (events
  11, 16) and the pane is back to the list, the network offered again, no check mark.
* A failed join is reported as one: in run t43 the second join, after the Forget, was sent to the 5 GHz access point of the network; the association was
  not acknowledged (event 7, status 5), the driver answered failure, and the pane showed "Unable to join the network". See the findings below for 5 GHz.

What had to be right for that (each is a finding below): the Wi-Fi row at all (MobileGestalt), the spinner (a packet filter tap), IPv6 multicast
(`allmulti`), and Settings itself (a Metal device for `iconservicesagent` and `Preferences`, and the copy of file-backed host buffers in the Mali driver).

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

* **Without `WLC_SET_INFRA` a join makes an ad-hoc network.** The firmware's default is IBSS: `bsscfg:join` then produced a link-up event with a
  random BSSID and never a key exchange, which looks like success in the event log. `WLC_SET_INFRA` 1 and `WLC_SET_AP` 0 before `WLC_UP` fix it
  (`brcmf_cfg80211_change_iface`).
* `bus:txglom` is set to 0 so the chip sends one frame per SDPCM packet: the reader does not unpack the superframes the firmware's default
  would batch.
* The firmware's own supplicant does WPA2-PSK: `bsscfg:wpa_auth` 0x80, `bsscfg:wsec` 4, `sup_wpa` 1 and the passphrase through
  `WLC_SET_WSEC_PMK` are enough; event 46 (`PSK_SUP`) with status 6 is the keyed state. The host handles no EAPOL.
* `cur_etheraddr` reads the module's own address from its OTP (`9c:b8:b4:...`), not the default in the nvram (`00:90:4c:27:80:01`), so
  the interface has a unique address with nothing to configure.
* **SDPCM flow control is credit based.** The transmit sequence starts at 255 and the credit window at 0 (as in `brcmfmac`); a frame is sent only
  while `tx_max - tx_seq` allows it, and the credits come back in the headers of whatever the chip sends. Under a bulk download the chip is busy
  with the receive side and the credits arrive slowly, so the transmit queue fills. With a 32-frame queue and no waiting, a run that received
  105 MB dropped 12006 of 38850 frames (acknowledgements among them). The queue now holds 128 frames and `if_output` waits up to 40 ms for room
  before it gives a frame up: in a run that received 8 MB, 1116 attempts found the queue full and were retried, and 19 of 2315 frames were
  finally given up. The 105 MB case was not repeated with the new code (the system downloaded little in the later runs).
* **The userland waits for IOKit to be quiet, and the original `AppleImage4` never lets it.** The network configuration (the interface
  namer) and the wireless daemon start their work after `IOKit` has been quiet (about five minutes of waiting, then they go on). With the
  Wi-Fi interface present the first lease came at 330 s. The cause was not the network: `AppleImage4` matches on `/defaults` and, in
  its `start`, waits for `IOAESAccelerator` without a timeout (found by reading its code in the kernel image: `finalize` calls
  `waitForMatchingService(serviceMatching("IOAESAccelerator"), UINT64_MAX)`). Nothing provided that class, so `defaults` stayed busy and
  the registry never went quiet (measured in the Wi-Fi runs; nothing in the cause is specific to them). UniXNU's software AES (`SoftwareAES`, the `IOAESAccelerator` of the ARM64 profile) is a plain
  AES-CBC in software and needs no SoC engine: with it (`CONFIG_XNU_NATIVE_AES=y` in `rk3588.config`, the `SoftwareAES` component and
  `build-software-aes-bindings.py` in `board.toml`) IOKit is quiet 26 s after start and the lease is there in the 73 s to 83 s status lines of the three
  runs after the change.
* `en0` needs `IFNET_INIT_NX_NOAUTO`, `bpf_attach` and the registry entries `IONetworkStack`, `IO80211Controller` and `IO80211Interface`
  before the userland treats it as the Wi-Fi interface; the three classes are registered late (see checkpoint 6).
* **Settings shows a Wi-Fi row only if MobileGestalt says the device has Wi-Fi, and in a virtual machine it says so from `/chosen`.** Settings
  hides the Wi-Fi specifier unless the `wifi` capability is true. With `vmm-present` (which `board.toml` sets for `watchdogd`) MobileGestalt reads the
  address from the property `mac-address-wifi0` of `/chosen`, which a boot loader fills in; without it the answer is a made-up address of zeros and the
  capability is false, although `en0` and `wifid` are fine. The board has no boot loader that knows the module, so `publish_registry()` in
  `WifiNet.cpp` sets the property from the module's own address once the firmware has reported it. MobileGestalt caches its answers on the Data volume
  (`com.apple.MobileGestalt.plist` under `systemgroup.com.apple.mobilegestaltcache`, written at the first boot), so a Data volume that once booted
  without the property keeps answering false until the file is deleted or the volume is rebuilt (`UniXNU/docs/guide/mobilegestalt.md`). The property is
  set only when the driver runs (`evb10-wifi=N`); a boot without it was not looked at after this change.
* **The row's spinner waits for the interface to stop being "busy", and that waits for a packet filter tap.** The wireless daemon shows the check
  mark when `IO80211InterfaceIPConfigurationBusy` goes false, and the address configuration clears it once DHCP and its ARP check of the router are done.
  The ARP monitor reads the router's replies through `/dev/bpf` on the interface, and for an interface that is not a stock Ethernet driver the stack
  gives the packet filter no frames of its own: the driver has to supply `set_bpf_tap` in `ifnet_init_eparams` and call the function it receives with every frame
  in both directions, Ethernet header included (`if_set_bpf_tap`, `on_rx` and `if_output` in `WifiNet.cpp`). `bpf_attach` alone only lets the DHCP client send.
  Without the tap the interface was busy for 64 s (run t36: true at 268.4 s, false at 332.9 s, the link up since 267.5 s); with it 4.5 to 9.5 s (runs t37, t42,
  t43). So the minute was a wait that ended by its own timeout (read from the disassembly of the address configuration daemon and confirmed by the
  fix), not a delay of the driver or the radio.
* **The firmware passes only the multicast groups it was told about** (`brcmf_netdev_set_multicast_list`), and the interface's list is not forwarded
  (`SIOCADDMULTI` is a no-op here), so IPv6 neighbour discovery never arrived. `allmulti` set to 1 after `mpc` fixes that (neighbour advertisements
  arrive from run t37 on). The test network sends no router advertisements at all, which the busy flag does not need once the tap is there.
* **Settings only starts if its icon agent has a Metal device.** `iconservicesagent` draws Settings' icons through Core Image, which without a Metal
  device falls back to OpenGL ES (this device has none) and ends in `CI::GLContext::init` with a jump to 0 (run t19). The plug-in's `clientProcess()`
  list (MaliMetal `ca77d29`) and the sandbox grant of the Mali driver (`package/Mali` `9dd949a`) name `iconservicesagent` and `Preferences`. The same driver
  commit makes a host buffer that sits on a file's pages (a mapped asset the render server shares with an application) a copy instead of a pin: the pin
  held the file object's activity count, and unmapping the file's last mapping waits for it in the thread that retires the GPU work that holds the pin.
  A build from the submodule commit before these two changes starts Settings and then leaves its main thread in `CASGetDisplays`, waiting for a reply from the
  render server that never comes (runs t39 to t41, `evb10-bt=Preferences` symbolised on the host); with them the pane is on the screen within 4 s of the tap.
  (A trap of this repository: `unixnu` resets `package/Mali` to the commit in the index, so a commit inside the submodule must be staged in
  `UniXNU-evb10` before the next build; runs t39 to t41 were that mistake.)
* **5 GHz does not work, and nothing here explains why yet.** After the firmware roamed to the 5 GHz access point of the test network
  (e2:05:a4:08:eb:ca) the TCP payload counter rose by 96 bytes in 30 s (188826 to 188922, run t42); a join that picked that access point after a Forget
  ended with an association nobody acknowledged (event 7, status 5, join result 3), which the pane shows as "Unable to join the network" (run t43). The
  scan RSSI of the 2.4 GHz access point differs from run to run (-44 dBm in t38, -80 dBm in t44 to t46) and the link reads -80 dBm. The CLM blob is
  not loaded (the file is data 1.39.1 of 2021, 5316 bytes; the firmware's built-in one is 9.7.7 of 2023) and the nvram has `ccode=0` and `regrev=0`;
  neither was tried. Locking the radio to 2.4 GHz (`WLC_SET_BAND` 2 before `WLC_UP`, as a boot argument that was removed again) is not a way around it: the
  scans then listed only 2412 MHz entries, but both joins of run t46 (265.8 s and 366.1 s) ended 0.5 s after the request with event 0, status 3 (no
  network found), where the first joins of runs t36, t37, t42 and t43, without the lock, took 1.7 to 1.8 s and succeeded. No panic in the 480 s.

## Firmware and board data

Recorded as optional entries in `proprietary-files.txt` (section "AP6398S Wi-Fi", source: the Rockchip Android vendor tree):

| File | What it is |
| --- | --- |
| `fw_bcm4359c0_ag.bin` | Ampak 4359c0 SDIO firmware 9.87.51.11.102 (2023-08-21); the chip is revision 9 |
| `nvram_ap6398s.txt` | `AP6398S_NVRAM_V4.2_20221130`, board type `0x0812`, board revision `0x1301`; it carries a default MAC address that the chip does not use (it reports the one in its OTP) |
| `4359_cypress_auto.clm_blob` | regulatory data (CLM); whether the C0 build wants it is not decided |

`fw_bcm4359c51a2_ag.bin` in the same directory is a 4362a2 build, not for this module. The kernel embeds the first two (checkpoint
3); the CLM blob is not loaded: the firmware starts without it (its built-in regulatory data was enough for the scans and the join above).

The network to join is not a repository file: `.cache/firmware/join.txt` (SSID, then passphrase if any; git-ignored) is embedded into the
kernel image by `scripts/build-wifi-firmware.py`. Without it levels 4 and 5 start and do not join.

## What is missing

1. **Speed.** The SDIO host is polled at about 25 MHz, with no DMA and no interrupt. The best 10 s interval of the 105 MB run carried 42.6 MB of
   TCP payload (34 Mbit/s) and the average over the traffic was about 5 Mbit/s, with the radio at -80 dBm on 2.4 GHz; whether the host or
   the radio is the limit was not separated. Still to come: the sample and drive phases in `SDIO_CON0/1` (0xfd7c0c24/28) and 50 MHz or more, the
   host's internal DMA for the frames, and the interrupt (SPI 204, a fixed number: the node must not export `interrupts`; function 1's
   in-band interrupt) instead of the polling worker. The eMMC host (`dwcmshc`, `Rk3588Emmc`) is a different controller.
2. **Settings, the rest.** The list, the join with a password, the status and Forget This Network work (checkpoint 7). Not tried: an open network, WPA3,
   a hidden network ("Other..."), the Wi-Fi switch of the pane after a join, and whether a network joined from the pane is remembered across
   boots (the boot-time join of the join file is still what joins unless `evb10-wifi-join=0` or `2`). The pane shows a "Private Wi-Fi Address" (a random
   one, 0E:...) that the driver does not program into the firmware: the router sees the module's own address (the DHCP replies are addressed to it).
3. **5 GHz, and roaming.** See the findings below: the firmware's roam to the 5 GHz access point of the test network stopped the traffic (run t42) and a join
   to it was not acknowledged (run t43). `WL_HOST_WAKE` (GPIO0_B2) and power save (`mpc` is 0, the radio stays on) are not used.
4. **Background traffic fills the Data window** within minutes of having a network (see the side effects of checkpoint 6). A device with
   a bigger Data volume or a daemon policy would not have this.
5. **Bluetooth** (UART9) is not part of this page.

## Next step

Find out why 5 GHz fails, because the firmware picks that access point on its own: load the CLM blob (or set the nvram's `ccode`/`regrev`), look at
the transmit power and the channel list the firmware reports, and join the 5 GHz access point alone (a BSSID in the join request) to separate the
association from the roam. Then make the data path fast: the interrupt and the DMA of the host, 50 MHz, and the transmit worker blocked on events
instead of polling. Not tried yet in the Settings pane: an open network, WPA3, a hidden network and a boot without `evb10-wifi`.
