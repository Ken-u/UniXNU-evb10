# Audio (ES8388 on I2S0) on the EVB10

Status: **implemented, three checkpoints** (on by default: `evb10-audio=4` and `evb10-audio-hal=1` are in `board.toml`). The kernel talks to the codec
over I2C and runs I2S0 as the master of a 48 kHz link with the codec as its clock slave (checkpoints 1 and 2). Checkpoint 3 connects it to iPadOS: a kernel
service, **Evb10Audio**, that `audiomxd` opens, and an audio server plug-in (CoreAudio's HAL interface) that is mounted over
`/System/Library/Audio/Plug-Ins/HAL`. On the board the audio server loads the plug-in, makes the device its output and input, starts it for the keyboard clicks
of a scripted touch run, and the frames reach the I2S0 transmit FIFO at 48 kHz with a few drop-outs. **Nobody has listened**: the evidence is the kernel's
frame accounting and the plug-in's counters over the serial line, not sound. The path has no DMA and no interrupt (a polled thread feeds the FIFO), so any
scheduling stall of the kernel over 0.7 ms is a drop-out. This page keeps what is known, what was observed on the board, and what comes next.

## Hardware

| Item | Value | Source |
| --- | --- | --- |
| Card | `es8388-sound` (`rockchip,multicodecs-card`, name `rockchip-es8388`), format `i2s`, `mclk-fs` 256 | stock device tree |
| Codec | Everest ES8388 at address 0x11 of `i2c@fec90000` (I2C7), MCLK 12.288 MHz (`assigned-clock-rates`) | stock device tree; the address answered on the board |
| I2C pins | `i2c7m0`: GPIO1_D0 = SCL, GPIO1_D1 = SDA, function 9 | stock device tree; `GPIO1D_L` read 0x99 before anything wrote it |
| Digital side | `i2s@fe470000` (I2S0, TDM-capable, `rockchip,trcm-sync-tx-only`), power domain 38 | stock device tree |
| I2S pins | MCLK GPIO1_C2, SCLK GPIO1_C3, LRCK GPIO1_C5 (function 1), SDO0 GPIO1_C7 (function 1), SDI0 GPIO1_D4 (function 2) | stock device tree |
| Amplifier enables, jack detect | `hp-con-gpio` GPIO4_A7 and `spk-con-gpio` GPIO4_A2 (both active high, GPIO4 at 0xfec50000), `hp-det-gpio` GPIO1_C4 (active low, GPIO1 at 0xfec20000); the banks' `pclk` gates are `CRU` 0x840 bit 14 and 0x844 bit 4 | stock device tree; the vendor `gpio-rockchip.c` register layout (DR 0x00, DDR 0x08 with the upper half-word as write mask, EXT_PORT 0x70) |
| Outputs | Headphone on LOUT1/ROUT1 (enable `hp-con-gpio`), speaker on LOUT2/ROUT2 (enable `spk-con-gpio`), headphone detect GPIO1_C4; inputs "Main Mic" on LINPUT1/2 and "Headset Mic" on RINPUT1/2 | stock device tree (`rockchip,audio-routing`) |
| Clock tree | `clk_i2s0_8ch_tx_src` (`CLKSEL_CON(24)`), its fractional divider (`CLKSEL_CON(25)`), the mux after it (`CLKSEL_CON(26)`), the MCLK out mux (`CLKSEL_CON(28)`), gates in `CLKGATE_CON(7)`; the clock to the MCLK pin is gated by `SYS_GRF` 0x318 bit 0 | Linux `clk-rk3588.c` and the device tree |

The board also has HDMI, DisplayPort and S/PDIF audio paths (`hdmi0-sound`, `dp0-sound`, `spdif-tx0-sound`); none of them is used here.

## Checkpoint 1: the kernel talks to the codec (done)

`evb10-audio=1`. `package/Evb10Audio/Evb10Audio.cpp` maps I2C7, the bus IOC and the CRU, opens the I2C7 clock gates if they are closed, routes
the pins, scans the bus, dumps registers 0x00 to 0x34 and writes and reads back one register. The I2C master is the polled one of U-Boot's
`rk_i2c.c` rewritten for the kernel, the same as `package/Evb10Touch` has for I2C5 (two copies: the touch driver's state is file-local).

Observed on the board (run a2, `evidence/audio-es8388-i2s0-20261101.txt`):

* The clock gates were open and the pins already routed (`GPIO1D_L` 0x99): the firmware leaves I2C7 ready, as it does I2C5.
* The scan finds exactly one device, 0x11, and all 53 register reads succeed.
* A write to `LOUT1VOL` (0x2e) reads back and is put back: PASS.
* The registers look like a configured codec, not a freshly reset one: the power registers are in a stopped state (`CHIPPOWER` 0xf3, `ADCPOWER` 0xff,
  `DACPOWER` 0xc0), while the formats (16 bit I2S, ratio 256), the mixers and `LOUT1VOL` / `ROUT1VOL` (0x1e) are set. That is presumably what an earlier
  Linux or Android boot left, because a board reset apparently does not cut the codec's supply (not checked). So the dump does not identify the chip by
  its reset values; the address, the 53 readable registers and the write check are what show that a codec answers.

## Checkpoint 2: I2S0 and the codec's clocks, a tone and a capture (done, the check that does not need a person)

`evb10-audio=2` (`evb10-audio-ms=N` sets the length, default 2000). After checkpoint 1 it looks at the audio power domain and stops without
touching the controller if the domain is off (a read from a block in a domain that is off hangs the bus). If it is on it:

1. opens `CLKGATE_CON(7)` bits 0, 1, 4, 5, 6, 7 and makes 12.288 MHz: GPLL (1188 MHz) divided by 4 gives 297 MHz, the fractional divider is 512/12375,
   the mux after it takes the fractional divider, the MCLK out mux takes the transmit clock, and `SYS_GRF` 0x318 bit 0 is cleared;
2. routes the five I2S pins;
3. programs I2S0 as the master (`CKR` with TRCM = transmit-only sync, 64 BCLK per LRCK; `CLKDIV` 4, so BCLK is 3.072 MHz), 16 bit valid data, two channels,
   I2S format, no DMA, no interrupt;
4. powers the codec as the vendor driver's bias levels do (VMID 5k, 100 ms, then 2 x 50k, `CHIPPOWER` 0, `ADCPOWER` 0, `DACPOWER` 0x3c) and sets slave mode, 16 bit
   I2S and MCLK = 256 fs for ADC and DAC;
5. fills the transmit FIFO from the thread with a 1 kHz sine (a quarter of full scale, a word with the same sample in both halves) for the length given, reads the
   receive FIFO into a statistic, stops the transfer and puts the codec's power registers back as they were.

Observed on the board (run a2, 2000 ms):

* The power domain was on already; GPLL read 1188000000 Hz; `CLKGATE_CON(7)` was 0 (nothing gated) before the write.
* The transmit FIFO took 96026 words in 2.000 s: **48012 words/s**. So the controller's own clocks run at 48 kHz and one FIFO word is one frame (both channels).
  No underrun or overrun flag (`INTSR` 0).
* The receive FIFO delivered 95998 words, **47998 words/s**, and the values vary within a small range (low half -968 to 8, high half -5 to 787, rms about
  236 and 188 on a 16 bit scale), neither all zero nor all ones nor the sine that goes out. The codec is a slave: it only sends ADC samples when it gets MCLK,
  BCLK and LRCK. So the clocks reach the codec and the frame format matches. What the samples are (a microphone's noise and offset is the likely reading) was
  not examined.
* No panic.

Not observed: sound. The codec's outputs carry the sine, but the external amplifier enables (`hp-con-gpio`, `spk-con-gpio`) are not driven and no person was at
the board, so whether anything is audible is not known.

## Checkpoint 3: the audio service and the audio server plug-in (done on the board, not listened to)

The design is that of UniXNU-sheng's audio for a DSP (its `ShengAudioService`, `ShengAudioPlugIn.c`, `ShengAudioAbi.h`, BSD-2-Clause-Patent, read as the
reference), with this board's I2S0 and its FIFOs in place of the DSP.

**Kernel side** (`package/Evb10Audio/Evb10Audio.cpp`, `evb10-audio=4`). After the bring-up of checkpoint 2 (without the tone) the driver publishes an
`IOService` of class `Evb10Audio` with a user client. The service property `NativeSandboxGrantProcess` (`audiomxd,mediaserverd`, `evb10-audio-grant`)
lets the audio server's sandbox open it (the same kernel patch that lets the render server open the Mali). The user client hands out three memory types:
the control page (`package/Evb10Audio/Evb10AudioAbi.h`), a playback ring (65536 stereo frames of 16 bit) and a capture ring (65536 mono frames), and takes four
requests: start and stop of each direction. One kernel thread at priority 95 does the rest, in a polled loop that wakes every 200 us:

* one timeline serves the whole boot: sample time 0 is the frame on the wire at the host time the service started, a frame lasts `ticks_per_frame_q32` host ticks
  (500: the timer is 24 MHz, which is also what CoreAudio assumes, and the I2S clocks and the timer come from the same crystal, so the two do not drift);
* it keeps the transmit FIFO (32 words deep, one word is one frame) at about 28 frames, taking frame `t` from the playback ring when it is at most 28 frames ahead
  of the wire, and clears the slot (the plug-in adds to what a slot holds, because the audio server runs more than one IO context on a device); if the wire has
  passed the next frame (the FIFO ran dry) it skips the frames and counts them;
* it drains the receive FIFO into the capture ring (the mean of the two channels), and publishes how far it got with a release store;
* the hardware (codec power, mixers and volumes, the amplifier GPIOs with the card's 30 ms and 40 ms delays, the I2S clocks) runs from the first start until
  `evb10-audio-linger` ms (default 3000) after the last stop; a start request returns once the hardware runs, because the plug-in places its timeline at that
  moment. The codec's mixers are set explicitly (DAC to mixer, volumes from `evb10-audio-vol`, default 28 of 36, 0 dB at 30), because the left-over state had the
  analogue bypass at its maximum.
* the amplifier is chosen by the headphone detect (low: a plug is in, `hp-con`; high: `spk-con`), or by `evb10-audio-out` (1 headphone, 2 speaker, 3 both).

**Audio server side** (`hal/Evb10AudioPlugIn.c`, built by `scripts/build-evb10-audio-hal.py`). A CFPlugIn bundle `Evb10Audio.driver` (flat: the executable and an
`Info.plist`) with one device, a 48 kHz stereo float output stream, a mono input stream and an output volume control (a squared curve over -36 dB to 0 dB). It
opens the service, maps the three memory types, mixes the audio server's output into the playback ring `lead_frames` plus a safety offset ahead of the play
position, and reads input from the capture ring. The device is declared with the transport type Thunderbolt, the answers to the private properties `accs` and
`auto`, empty lists for the custom-property, box and clock lists, and zero-time stamps over a 65536-frame period, because the audio server drops a device
that answers an unknown property with an error; these were learned by UniXNU-sheng's bring-up and are used as they found them. The plug-in also records in the
control page what the audio server asked for and it did not answer (`[props]`), every distinct property access in order (`[trace N]`, including the failing calls
of the audio server's own routing layer, which it rebinds to see), the write-ahead and read-behind of the IO cycles, its IO thread's priority and time constraint,
and the host clock frequency the audio server reports (24 MHz; nothing is changed). The kernel prints all of it from a separate thread, because a kernel printf
(1.5 Mbaud, about 0.7 ms a line) in the engine thread starves the FIFO.

**Delivery.** `scripts/build-evb10-audio-hal.py` builds the bundle with the iOS SDK and clang 20 (ad hoc signed, 4 KiB pages) and packs it as an NGPUADD2 addon
(`audio/Evb10Audio.driver/*`); `uefi-evb10`'s `tools/board/make-payload-boot.sh` merges it (`merge-addons.py`) into the Metal plug-in's addon while `board.toml`
has `evb10-audio-hal=1`; the kernel writes it to `/private/var/ngpu/audio` on the Data volume and union-mounts that read-only (bindfs) over
`/System/Library/Audio/Plug-Ins/HAL`, where the audio server loads every driver bundle (`patches/tools/evb10-userland-mounts.c.inc`: the second mount next to the
Metal plug-in's, and an `mmap` grant for its files). The executable's CDHash is in the owner trust cache: MaliMetal's `usertest/build.sh` takes
`out/audio/Evb10Audio.driver/Evb10Audio` into it and pins the bundle in `board.toml`. Apple's `AudioServerPlugIn.h`, `AudioHardwareBase.h` and `AudioHardware.h` are not
in the iOS SDK and are not stored here: the script fetches them from a macOS 11.3 SDK mirror into `.cache/coreaudio-headers`, checked against pinned hashes.

Boot arguments: `evb10-audio=4` (the service), `evb10-audio-hal=1` (the plug-in; with `evb10-gpu-addon=1`), `evb10-audio-out`, `evb10-audio-vol`, `evb10-audio-linger`,
`evb10-audio-grant`, and `evb10-audio-selftest=N`, which makes the service play a 480 Hz triangle through its own rings for N seconds and record, as the plug-in
would, without the audio server.

Observed on the board (`evidence/audio-ipados-20261101.txt`, runs aud1 to aud3; aud4 is the same boot without extra arguments):

* **The engine alone** (aud1, the self-test): 48000 frames/s on both FIFOs (239985 transmit and 239980 receive frames in 5 s), the thread's wake-up gaps all
  under 300 us but two of 2.1 ms in 13 s, each of which ran the FIFO dry once (75 frames skipped).
* **The first version's own log was the largest glitch** (aud2): the engine thread printed the statistics and the 106 trace lines itself and stalled for 68 ms
  (3249 frames skipped) at the first print, and 2 to 5 ms at every 5 s report. The prints moved to a separate thread.
* **iPadOS** (aud3, scripted touches that type the Wi-Fi password, so the sounds are keyboard clicks): the mount shows the plug-in next to the sealed
  `BuiltinAudioPlugin.driver` (stage 5), `audiomxd` opens the service at 78.3 s and the plug-in loads, the audio server reads the device's properties (one of its
  own queries, `cfsz`, is not answered; the device stays), sets the output volume (225/1000), and calls start at 233.1 s. The hardware comes on (headphone amplifier;
  the detect pin read 0), and in the 39 s of the run the audio server wrote 1024-frame blocks every 21 ms (1568 of 1571 intervals in 15 to 25 ms), always ahead
  of the play position (**write-ahead 1531 to 2049 frames**, never negative), from an IO thread at priority 97 with a time constraint (period 512000). The plug-in's
  mix peak was 8748 of 32767 before the volume and **the largest sample put into the transmit FIFO was 727**, which is 8748 times the volume curve's -21.6 dB, so
  the sound of iPadOS goes through the plug-in, the ring and the service into the I2S0 FIFO with the volume applied. The audio server reports a 24 MHz host clock.
  Transmit and receive both ran at 48 kHz (1890782 and 1890791 frames over the run's hardware time), `underruns 0, overruns 0`. The wake-up gaps were under 300 us
  in 171901 of 171906 ticks; four times it ran over 2 ms (longest 4.1 ms) and the FIFO ran dry, 643 frames in all (13.4 ms of 39 s, 4 drop-outs); the audio server
  itself left two stretches of its writes out (5156 frames in all, the last at the end of the run: its IO cycles paused). The capture ring filled with the codec's input (peaks of 45 to 90)
  and nobody read it.
* No panic in the three runs.

Not observed: **sound**; whether the headphone detect polarity is right (the pin read 0 with nothing known to be plugged in, so the headphone amplifier was
chosen: `evb10-audio-out=3` drives both); the quality of the capture; an application recording from the microphone; anything but the keyboard clicks (system
sounds, media playback, a second client). The 4 drop-outs in 39 s are the kernel scheduling the thread late, most likely the display and GPU work around the
touches; they are not corrected.

## What is missing

1. **DMA and interrupt.** The controller is fed by a polled thread; the 32-word FIFO (667 us) is the only tolerance of any kernel scheduling stall. The device
   tree names DMA channels and SPI 180 (0xb4). With the PL330 the ring could be played from memory and the thread would only be needed for the timeline.
2. **Listening.** A person with headphones or a speaker should check: the keyboard clicks, a tone (`evb10-audio-selftest=10`), the jack detect polarity
   (`evb10-audio-out=3` plays on both), the volume curve, pops at start and stop.
3. **The power domain.** Domain 38 was on at the firmware's hand-over; this driver does not turn it on (the Mali driver shows how: the repair status, the idle request).
4. **Other rates and formats**, capture routing (the capture settings are set explicitly: `ADCCONTROL2` 0xf8 differential inputs as the left-over state had, input gain
   0), the headphone detect as an event instead of at the start of the hardware, HDMI / DisplayPort / S/PDIF audio.
5. **Video codecs** are a separate question: iPadOS decodes H.264 and HEVC through the kernel service behind `AppleAVDUserClient` (named in the entitlements of
   `videodecodeservice` and `videocodecd`), which this kernel does not have; the board has `rkvdec`, `rkvenc`, `vepu`, `jpegd` and an AV1 decoder block. Only the
   AV1 software decoder (`/System/Library/VideoDecoders/AV1SW.metallib`, a Metal program) is in the OS image as a fallback. Nothing was tried.

## Next step

Someone with a speaker or headphones listens (`evb10-audio-selftest=10` makes a tone without iPadOS; the keyboard clicks need the scripted touch run of
`docs/network.md`). Then DMA for the transmit and receive rings, which removes the drop-outs.
