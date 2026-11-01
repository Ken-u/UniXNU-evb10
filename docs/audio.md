# Audio (ES8388 on I2S0) on the EVB10

Status: **implemented, two checkpoints** (opt-in with a boot argument). The kernel talks to the codec over I2C, and it runs the I2S0 controller
as the master of a 48 kHz link with the codec as its clock slave: the transmit FIFO drains at the 48 kHz frame rate and the receive FIFO delivers
the codec's ADC samples in step with it. **Nothing of this is connected to iPadOS yet**: there is no audio service for CoreAudio, so `audiomxd`
still runs without an output device, and nobody has listened to the tone. This page keeps what is known, what was observed on the board, and
what comes next.

## Hardware

| Item | Value | Source |
| --- | --- | --- |
| Card | `es8388-sound` (`rockchip,multicodecs-card`, name `rockchip-es8388`), format `i2s`, `mclk-fs` 256 | stock device tree |
| Codec | Everest ES8388 at address 0x11 of `i2c@fec90000` (I2C7), MCLK 12.288 MHz (`assigned-clock-rates`) | stock device tree; the address answered on the board |
| I2C pins | `i2c7m0`: GPIO1_D0 = SCL, GPIO1_D1 = SDA, function 9 | stock device tree; `GPIO1D_L` read 0x99 before anything wrote it |
| Digital side | `i2s@fe470000` (I2S0, TDM-capable, `rockchip,trcm-sync-tx-only`), power domain 38 | stock device tree |
| I2S pins | MCLK GPIO1_C2, SCLK GPIO1_C3, LRCK GPIO1_C5 (function 1), SDO0 GPIO1_C7 (function 1), SDI0 GPIO1_D4 (function 2) | stock device tree |
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

## What is missing

1. **An audio service for iPadOS.** CoreAudio needs a provider: a kernel service with the stream engine and a HAL plug-in that is union-mounted over
   `/System/Library/Audio/Plug-Ins/HAL`, as UniXNU's other devices do (`UniXNU/docs/porting/power.md`, "Implement audio streams"; the CoreAudio host clock
   constant has to be corrected when the timebase is not 24 MHz). The HAL directory has `BuiltinAudioPlugin`, `BTAudioHALPlugin`, `AppleUSBAudio` and
   others, none of them for this codec. `audiomxd`, `mediaserverd` and `videocodecd` start without errors in the serial log.
2. **DMA and interrupt.** The controller is fed by polling; the tone needs a busy thread. The device tree names DMA channels and SPI 180 (0xb4).
3. **The output enables and jack detection:** `hp-con-gpio`, `spk-con-gpio`, `hp-det-gpio` with its ADC key, volumes and routes as the card describes them.
4. **The power domain.** Domain 38 was on at the firmware's hand-over; this driver does not turn it on (the Mali driver shows how: the repair status, the idle request).
5. **Other rates and formats**, capture routing (the left-over `ADCCONTROL2` 0xf8 selects differential inputs), HDMI / DisplayPort / S/PDIF audio.
6. **Video codecs** are a separate question: iPadOS decodes H.264 and HEVC through the kernel service behind `AppleAVDUserClient` (named in the entitlements of
   `videodecodeservice` and `videocodecd`), which this kernel does not have; the board has `rkvdec`, `rkvenc`, `vepu`, `jpegd` and an AV1 decoder block. Only the
   AV1 software decoder (`/System/Library/VideoDecoders/AV1SW.metallib`, a Metal program) is in the OS image as a fallback. Nothing was tried.

## Next step

The audio service: a kernel `IOService` over this driver's I2S and codec code with a stream engine, DMA instead of polling, and the HAL plug-in; the first
checkpoint there is a PCM stream from `audiomxd` reaching the transmit FIFO (the same 48 kHz accounting). In parallel, a person with a speaker or headphones
can check the tone: `evb10-audio=2` plus driving the two enable GPIOs.
