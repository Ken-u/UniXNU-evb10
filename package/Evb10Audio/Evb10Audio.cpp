// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Evb10Audio: the audio codec of the Rockchip RK3588 EVB10 (docs/audio.md).
//
// The board's `es8388-sound` card has an Everest ES8388 at address 0x11 of I2C7 (0xfec90000, pins GPIO1_D0 = SCL / GPIO1_D1 = SDA, function 9,
// the "i2c7m0" routing of the board's device tree) and the I2S0 controller (0xfe470000, power domain 38) as its digital side, with the codec as the
// clock slave: MCLK 12.288 MHz from the SoC (GPIO1_C2), BCLK / LRCK from the controller.
//
// Checkpoint 1 (evb10-audio=1): the kernel talks to the codec. Once, from a worker thread: opens the I2C7 clock gates if the firmware left them
// closed, routes the two pins, scans the bus, dumps the codec's registers 0x00..0x34 and checks that a write reads back (LOUT1VOL, restored
// afterwards). An acknowledged address and a register that holds a value is what "the codec answers" means here.
//
// Checkpoint 2 (evb10-audio=2): the I2S side, polled (no DMA, no interrupt). After checkpoint 1: looks at the audio power domain (and stops
// without touching the controller if it is off: a read of a block in a domain that is off hangs the bus), opens the audio clock gates, makes
// 12.288 MHz from GPLL (CLK_I2S0_8CH_TX_SRC divider and the fractional divider), turns the clock out to the MCLK pin, routes the five I2S pins,
// programs the controller as the master of a 48 kHz, 16 bit, two channel I2S link, powers the codec for playback and capture, and for a few
// seconds feeds the transmit FIFO with a 1 kHz sine from the thread while reading the receive FIFO. What it measures, and why it counts:
//   * how many words the transmit FIFO took per second: 48000 means the controller's own clocks run at 48 kHz and a word is a frame
//     (both channels), 96000 would be a word per channel, close to 0 means the FIFO does not drain (no clock);
//   * what the receive FIFO delivered: the codec is a slave, so it only sends samples of its ADC when it gets MCLK, BCLK and LRCK; non-zero
//     samples (a microphone's noise) therefore show that the clocks reach the codec and the format matches. That is the check that does not
//     need a person listening. The sine itself goes to the headphone and speaker outputs of the codec, whose external amplifier enables
//     (hp-con / spk-con GPIOs of the device tree) are not driven yet: nobody has listened to it.
// The codec's registers are put back as they were afterwards.
//
// Checkpoint 3 (evb10-audio=4): iPadOS. After the bring-up of checkpoint 2 (without the tone) the kernel publishes an IOService, "Evb10Audio", with a user
// client whose memory types are a control page and two sample rings (Evb10AudioAbi.h), the way UniXNU-sheng's audio service does for a DSP. The audio server
// (audiomxd) loads a CoreAudio plug-in, hal/Evb10AudioPlugIn.c, union-mounted over /System/Library/Audio/Plug-Ins/HAL by the userland-mounts patch
// (boot-arg evb10-audio-hal=1), which opens the service, starts and stops each direction and mixes the audio server's output into the playback ring. A kernel
// thread (priority 95, polled: no DMA, no interrupt) does what a DSP would: while a direction is started it keeps the I2S0 transmit FIFO topped up with frames
// that are at most 28 ahead of the wire, takes them from the playback ring (and clears them), and drains the receive FIFO into the capture ring. The codec,
// its mixers and the two output amplifier enables (hp-con GPIO4_A7, spk-con GPIO4_A2, chosen by the headphone detect GPIO1_C4) are on only while the hardware
// runs, which is from the first start until evb10-audio-linger milliseconds after the last stop.
//
// The I2C master is the polled one of U-Boot's rk_i2c.c (GPL-2.0+, Rockchip) rewritten for the kernel, the same as package/Evb10Touch has for
// I2C5; the two are separate copies because the touch driver's state is file-local and it is in use. The clock and pin numbers are those of
// the vendor kernel's clk-rk3588.c, rockchip_i2s_tdm.c and the board's device tree. The service, the user client and the shared-memory layout follow
// UniXNU-sheng's ShengAudioService.cpp and AudioProbe.cpp (BSD-2-Clause-Patent, Copyright (c) 2026, UniXNU contributors).
//
// Boot-args: evb10-audio=N       1: checkpoint 1; 2: checkpoints 1 and 2; 4: checkpoint 3 (the service)
//            evb10-audio-addr=N  the codec's 7 bit address (default 0x11)
//            evb10-audio-khz=N   the SCL rate, 100..400 (default 100)
//            evb10-audio-ms=N    how long the tone and the capture of checkpoint 2 run (default 2000, 100..10000)
//            evb10-audio-out=N   the output: 0 by the headphone detect (default), 1 headphone, 2 speaker, 3 both
//            evb10-audio-vol=N   the codec's output volume registers, 0..36 (30 is 0 dB, 1.5 dB a step; default 28)
//            evb10-audio-linger=N  milliseconds the hardware stays on after the last stop (default 3000)
//            evb10-audio-selftest=N  with evb10-audio=4: the service plays a 480 Hz triangle through its own rings for N seconds (1..120) and records
//                                    from the input, as the plug-in would; the log shows the FIFO accounting and the peaks
#include <IOKit/IOLib.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSMetaClass.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdint.h>
#include <string.h>
#include "Evb10AudioAbi.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
// (kern/thread.h is not included: it changes how IOLib.h's typed allocators compile under -Werror)
extern "C" kern_return_t kernel_thread_start(thread_continue_t continuation, void *parameter, thread_t *new_thread);
extern "C" kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority, thread_t *new_thread);
extern "C" void thread_set_thread_name(thread_t thread, const char *name);
extern "C" void thread_deallocate(thread_t thread);
extern "C" kern_return_t thread_terminate(thread_t thread);
extern "C" thread_t current_thread(void);

namespace {

constexpr uint64_t kI2c7 = 0xfec90000ULL;
constexpr uint64_t kBusIoc = 0xfd5f8000ULL;
constexpr uint64_t kCru = 0xfd7c0000ULL;

// I2C registers (rk3399-style controller)
enum : uint32_t {
  kCon = 0x00, kClkDiv = 0x04, kMrxAddr = 0x08, kMrxRaddr = 0x0c, kMtxCnt = 0x10, kMrxCnt = 0x14, kIen = 0x18, kIpd = 0x1c,
  kTxData = 0x100, kRxData = 0x200,
};
enum : uint32_t {
  kConEn = 1u << 0, kConStart = 1u << 3, kConStop = 1u << 4, kConLastAck = 1u << 5, kConActAck = 1u << 6,
  kModeTx = 0, kModeTrx = 1, kModeRx = 2,
  kIntMbtf = 1u << 2, kIntMbrf = 1u << 3, kIntStart = 1u << 4, kIntStop = 1u << 5, kIntNak = 1u << 6, kIntAll = 0x7f,
};
constexpr unsigned kFifo = 32;

volatile uint32_t *gI2c, *gIoc, *gCru;

inline uint32_t rd(volatile uint32_t *base, uint32_t off) { return base[off / 4]; }
inline void wr(volatile uint32_t *base, uint32_t off, uint32_t value) { base[off / 4] = value; }

uint64_t nowUs() {
  uint64_t ns;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000;
}

uint32_t divUp(uint64_t a, uint64_t b) { return static_cast<uint32_t>((a + b - 1) / b); }

// ---------------------------------------------------------------------------------------------------------------- I2C

struct I2c {
  unsigned cfg = 0;
  static constexpr uint64_t kTimeoutUs = 100000;

  void disable() {
    wr(gI2c, kIen, 0);
    wr(gI2c, kIpd, kIntAll);
    wr(gI2c, kCon, 0);
  }

  // rk_i2c_adapter_clk of U-Boot (controllers with the version field set, which this one has).
  bool setSpeed(uint32_t sclRate, uint32_t clkRate) {
    unsigned startSetup = 0;
    uint32_t minLowNs, minHighNs, maxRiseNs, maxFallNs;
    if (sclRate <= 100000 && sclRate >= 1000) {
      startSetup = 1; minLowNs = 4700; minHighNs = 4000; maxRiseNs = 1000; maxFallNs = 300;
    } else if (sclRate <= 400000 && sclRate >= 100000) {
      minLowNs = 1300; minHighNs = 600; maxRiseNs = 300; maxFallNs = 300;
    } else {
      return false;
    }
    const uint64_t rateKhz = divUp(clkRate, 1000);
    const uint64_t sclKhz = divUp(sclRate, 1000);
    const uint32_t minTotal = divUp(rateKhz, sclKhz * 8);
    uint32_t minHigh = divUp(rateKhz * (maxRiseNs + minHighNs), 8ULL * 1000000);
    uint32_t minLow = divUp(rateKhz * (maxFallNs + minLowNs), 8ULL * 1000000);
    minHigh = minHigh < 1 ? 2 : minHigh;
    minLow = minLow < 1 ? 2 : minLow;
    const uint32_t minHold = minHigh + minLow;
    uint32_t high, low;
    if (minHold >= minTotal) {
      high = minHigh; low = minLow;
    } else {
      const uint32_t extra = minTotal - minHold;
      const uint32_t extraLow = divUp(static_cast<uint64_t>(minLow) * extra, minHold);
      low = minLow + extraLow;
      high = minHigh + (extra - extraLow);
    }
    high--; low--;
    if (high > 0xffff || low > 0xffff) return false;
    cfg = (1u << 8) | (startSetup << 12);  // SDA_CFG(1) | STA_CFG(start_setup)
    wr(gI2c, kClkDiv, (high << 16) | low);
    return true;
  }

  int waitIpd(uint32_t bit, uint32_t alsoNak) {
    const uint64_t began = nowUs();
    for (;;) {
      const uint32_t ipd = rd(gI2c, kIpd);
      if ((alsoNak & kIntNak) && (ipd & kIntNak)) {
        wr(gI2c, kIpd, kIntNak);
        return -2;  // no acknowledge
      }
      if (ipd & bit) {
        wr(gI2c, kIpd, bit);
        return 0;
      }
      if (nowUs() - began > kTimeoutUs) return -1;
      IODelay(1);
    }
  }

  int startBit(uint32_t con) {
    wr(gI2c, kIpd, kIntAll);
    wr(gI2c, kIen, kIntStart);
    wr(gI2c, kCon, kConEn | kConStart | cfg | con);
    const int r = waitIpd(kIntStart, 0);
    if (r) return r;
    wr(gI2c, kCon, kConEn | cfg | con);  // clean the start bit
    return 0;
  }

  int stopBit() {
    wr(gI2c, kIpd, kIntAll);
    wr(gI2c, kCon, kConEn | cfg | kConStop);
    wr(gI2c, kIen, kIntStop);
    const int r = waitIpd(kIntStop, 0);
    IODelay(1);
    return r;
  }

  // The address byte is part of the transmit count: a write of zero bytes is an address probe.
  int write(uint8_t chip, const uint8_t *buf, unsigned len) {
    unsigned remain = len + 1;
    bool first = true, next = false;
    while (remain) {
      const unsigned now = remain > kFifo ? kFifo : remain;
      const unsigned words = (now + 3) / 4;
      for (unsigned i = 0; i < words; ++i) {
        uint32_t data = 0;
        for (unsigned j = 0; j < 4; ++j) {
          if (i * 4 + j == now) break;
          if (first && i == 0 && j == 0) data |= static_cast<uint32_t>(chip) << 1;
          else data |= static_cast<uint32_t>(*buf++) << (j * 8);
        }
        wr(gI2c, kTxData + i * 4, data);
      }
      first = false;
      if (!next) {
        const int r = startBit(kConEn | (kModeTx << 1));
        if (r) return r;
        next = true;
      } else {
        wr(gI2c, kCon, kConEn | (kModeTx << 1) | cfg);
      }
      wr(gI2c, kIen, kIntMbtf | kIntNak);
      wr(gI2c, kMtxCnt, now);
      const int r = waitIpd(kIntMbtf, kIntNak);
      if (r) return r;
      remain -= now;
    }
    return 0;
  }

  // snd: this read follows a write without a stop, so the controller is reset and sends a repeated start.
  int read(uint8_t chip, uint8_t *buf, unsigned len, bool snd) {
    unsigned remain = len;
    bool chunk = false;
    if (snd) wr(gI2c, kCon, 0);
    wr(gI2c, kMrxAddr, (1u << 24) | (static_cast<uint32_t>(chip) << 1) | 1u);
    wr(gI2c, kMrxRaddr, 0);
    while (remain) {
      uint32_t con;
      unsigned now;
      if (remain > kFifo) {
        con = kConEn;
        now = kFifo;
      } else {
        con = kConEn | kConLastAck;
        now = remain;
      }
      const unsigned words = (now + 3) / 4;
      if (chunk) {
        con |= kModeRx << 1;
        wr(gI2c, kCon, con | cfg);
      } else {
        con |= kModeTrx << 1;
        const int r = startBit(con);
        if (r) return r;
      }
      wr(gI2c, kIen, kIntMbrf | kIntNak);
      wr(gI2c, kMrxCnt, now);
      const int r = waitIpd(kIntMbrf, kIntNak);
      if (r) return r;
      for (unsigned i = 0; i < words; ++i) {
        const uint32_t data = rd(gI2c, kRxData + i * 4);
        for (unsigned j = 0; j < 4; ++j) {
          if (i * 4 + j == now) break;
          *buf++ = static_cast<uint8_t>(data >> (j * 8));
        }
      }
      remain -= now;
      chunk = true;
    }
    return 0;
  }

  // Write, then (after a repeated start) read; always ends with a stop.
  int xfer(uint8_t chip, const uint8_t *wbuf, unsigned wlen, uint8_t *rbuf, unsigned rlen) {
    int r = 0;
    cfg |= kConActAck;
    if (wlen) r = write(chip, wbuf, wlen);
    if (!r && rlen) r = read(chip, rbuf, rlen, wlen != 0);
    stopBit();
    disable();
    return r;
  }
  bool probe(uint8_t chip) {
    uint8_t dummy = 0;
    cfg |= kConActAck;
    const int r = write(chip, &dummy, 0);
    stopBit();
    disable();
    return r == 0;
  }
} gBus;

// ---------------------------------------------------------------------------------------------------------------- the codec

uint8_t gAddr = 0x11;

int codecRead(uint8_t reg, uint8_t *value) { return gBus.xfer(gAddr, &reg, 1, value, 1); }
int codecWrite(uint8_t reg, uint8_t value) {
  const uint8_t w[2] = {reg, value};
  return gBus.xfer(gAddr, w, 2, nullptr, 0);
}

uint8_t gRegs[0x35];  // the dump at the start, which is what is put back

// ---------------------------------------------------------------------------------------------------------------- bring-up of the I2C side

// The clock of I2C7 (clk-rk3588.c of the vendor kernel, the pattern of I2C5 in package/Evb10Touch): CLKSEL_CON(38) bit 12 chooses 200 / 100
// MHz, CLKGATE_CON(10) bit 14 gates its pclk and CLKGATE_CON(11) bit 6 the functional clock. The firmware may leave them running; only a
// gate found closed is opened. The log shows what was found.
uint32_t clockRate() {
  const uint32_t sel = rd(gCru, 0x398), g10 = rd(gCru, 0x828), g11 = rd(gCru, 0x82c);
  printf("evb10: audio: CRU sel38 %08x gate10 %08x gate11 %08x (i2c7: %s MHz, pclk %s, clk %s)\n", sel, g10, g11,
         (sel >> 12) & 1 ? "100" : "200", (g10 >> 14) & 1 ? "GATED" : "on", (g11 >> 6) & 1 ? "GATED" : "on");
  if ((g10 >> 14) & 1) wr(gCru, 0x828, 1u << (14 + 16));
  if ((g11 >> 6) & 1) wr(gCru, 0x82c, 1u << (6 + 16));
  return (sel >> 12) & 1 ? 100000000u : 200000000u;
}

// Pins are nibbles of BUS_IOC's GPIOnX_IOMUX_SEL_L / _H registers (bank 1 starts at 0x20, eight bytes per bank half: A_L 0x20, A_H 0x24, B_L 0x28,
// B_H 0x2c, C_L 0x30, C_H 0x34, D_L 0x38, D_H 0x3c; the touch driver's GPIO1B_H at 0x2c has the same layout). The write has the upper half as its mask.
void iocSet(uint32_t off, uint32_t mask, uint32_t bits, const char *what) {
  const uint32_t before = rd(gIoc, off);
  if ((before & mask) != (bits & mask)) wr(gIoc, off, (mask << 16) | (bits & mask));
  printf("evb10: audio: IOC %02x (%s) %08x -> %08x\n", off, what, before, rd(gIoc, off));
}

void i2cPins() {
  // GPIO1_D0 / D1: function 9 is I2C7
  iocSet(0x38, 0x00ff, 0x0099, "GPIO1D_L: D0 D1 = I2C7");
}

void dumpRegisters() {
  unsigned failed = 0;
  for (unsigned r = 0; r < sizeof gRegs; ++r) {
    if (codecRead(static_cast<uint8_t>(r), &gRegs[r])) ++failed;
  }
  for (unsigned r = 0; r < sizeof gRegs; r += 8) {
    char line[96];
    unsigned at = 0;
    for (unsigned i = r; i < r + 8 && i < sizeof gRegs; ++i) at += static_cast<unsigned>(snprintf(line + at, sizeof line - at, " %02x", gRegs[i]));
    printf("evb10: audio: regs %02x:%s\n", r, line);
  }
  printf("evb10: audio: register dump: %u of %u reads failed\n", failed, static_cast<unsigned>(sizeof gRegs));
}

// LOUT1VOL (0x2e, 6 bits) holds what is written: write another value, read it back, put the old one back.
void writeCheck() {
  constexpr uint8_t kReg = 0x2e;
  uint8_t before = 0, after = 0, restored = 0;
  int r = codecRead(kReg, &before);
  if (r) {
    printf("evb10: audio: write check: first read of %02x failed (%d)\n", kReg, r);
    return;
  }
  const uint8_t test = (before & 0x3f) == 0x15 ? 0x0a : 0x15;
  const int w = codecWrite(kReg, test);
  r = codecRead(kReg, &after);
  const int w2 = codecWrite(kReg, before);
  const int r2 = codecRead(kReg, &restored);
  printf("evb10: audio: write check reg %02x: was %02x, wrote %02x (%d), read %02x (%d), put back %02x (%d), read %02x (%d): %s\n", kReg, before,
         test, w, after, r, before, w2, restored, r2,
         (!w && !r && !w2 && !r2 && (after & 0x3f) == test && restored == before) ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------------------------------------------- the I2S side

constexpr uint64_t kPmuPhys = 0xfd8d8000ULL, kGrfPhys = 0xfd58c000ULL, kI2s0Phys = 0xfe470000ULL;
volatile uint32_t *gPmu, *gGrf, *gI2s;

// PMU, vendor "rk3588_pmu" (the Mali driver has the same table): the audio domain (38) is bit 4 of the power register 0x14c + 4, bit 1 of the idle
// request 0x10c + 4, bit 17 of ack 0x118 and idle 0x120, bit 19 of the repair status 0x290 (on); its memory status is bit 28 of 0x1a8 (clear = on).
enum : uint32_t { kPmuReq = 0x110, kPmuAck = 0x118, kPmuIdle = 0x120, kPmuPwr = 0x150, kPmuMem = 0x1a8, kPmuRepair = 0x290 };
constexpr uint32_t kPdOn = 1u << 19;  // only the repair status is looked at: this driver does not power the domain

// CRU (clk-rk3588.c): CLKSEL_CON(24) bit 9 and [8:4] are clk_i2s0_8ch_tx_src (GPLL / AUPLL, divider - 1), CLKSEL_CON(25) the fractional divider
// (numerator << 16 | denominator), CLKSEL_CON(26) [1:0] the mux after it (1 = the fractional divider), CLKSEL_CON(28) [3:2] the MCLK out mux (0 = the
// transmit clock); CLKGATE_CON(7) bit 0 hclk_audio_root, 1 pclk_audio_root, 4 hclk_i2s0_8ch, 5 / 6 / 7 the transmit source, fractional divider and
// mclk_i2s0_8ch_tx (1 = gated). SYS_GRF 0x318 bit 0 (set = off) gates the clock on its way to the MCLK pin.
enum : uint32_t { kSel24 = 0x360, kSel25 = 0x364, kSel26 = 0x368, kSel28 = 0x370, kGate7 = 0x81c, kGrfMclkOut = 0x318 };
constexpr unsigned kGpllCon = 112;  // CRU word index of GPLL's CON0

// I2S-TDM controller (rockchip_i2s_tdm.h)
enum : uint32_t {
  kIsTxcr = 0x00, kIsRxcr = 0x04, kIsCkr = 0x08, kIsTxFifo = 0x0c, kIsDmacr = 0x10, kIsIntcr = 0x14, kIsIntsr = 0x18, kIsXfer = 0x1c,
  kIsClr = 0x20, kIsTxdr = 0x24, kIsRxdr = 0x28, kIsRxFifo = 0x2c, kIsTdmTx = 0x30, kIsTdmRx = 0x34, kIsClkdiv = 0x38,
};

inline void dsb() { __asm__ volatile("dsb sy" ::: "memory"); }

void maskWrite(volatile uint32_t *base, uint32_t off, uint32_t mask, uint32_t bits) {
  wr(base, off, (mask << 16) | (bits & mask));
  dsb();
}

uint64_t pllHz(unsigned con) {
  const uint32_t c0 = gCru[con], c1 = gCru[con + 1], c2 = gCru[con + 2];
  const uint64_t m = c0 & 0x3ff, p = c1 & 0x3f, s = (c1 >> 6) & 7, k = c2 & 0xffff;
  if (!p || (c1 & (1u << 13))) return 0;  // no divider, or powered down
  return ((24000000ull * (m * 65536 + k)) / 65536 / p) >> s;
}

uint64_t gcd(uint64_t a, uint64_t b) {
  while (b) {
    const uint64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

uint32_t pmu(uint32_t off) { return rd(gPmu, off); }

bool audioDomainOn() {
  const bool on = (pmu(kPmuRepair) & kPdOn) != 0;
  printf("evb10: audio: power domain 38 is %s (pwr %08x req %08x ack %08x idle %08x repair %08x mem %08x)\n", on ? "on" : "OFF", pmu(kPmuPwr),
         pmu(kPmuReq), pmu(kPmuAck), pmu(kPmuIdle), pmu(kPmuRepair), pmu(kPmuMem));
  return on;
}

// 12.288 MHz (256 x 48 kHz) from GPLL: the source divider brings GPLL near 300 MHz (the fractional divider wants its input at least 20 times
// its output), and the fraction is the exact ratio, reduced until it fits 16 bits.
bool startMclk() {
  const uint32_t mclk = 12288000;
  const uint64_t gpll = pllHz(kGpllCon);
  if (!gpll) {
    printf("evb10: audio: GPLL is off\n");
    return false;
  }
  uint32_t div = static_cast<uint32_t>(gpll / (20ull * mclk));
  if (div < 1) div = 1;
  if (div > 32) div = 32;
  const uint64_t src = gpll / div;
  const uint64_t g = gcd(mclk, src);
  uint64_t n = mclk / g, m = src / g;
  while (m > 0xffff || n > 0xffff) {
    n >>= 1;
    m >>= 1;
  }
  const uint32_t gate = rd(gCru, kGate7);
  printf("evb10: audio: GPLL %llu Hz, source /%u = %llu Hz, fraction %llu/%llu -> %llu Hz; CLKGATE_CON(7) was %08x\n", static_cast<unsigned long long>(gpll),
         div, static_cast<unsigned long long>(src), static_cast<unsigned long long>(n), static_cast<unsigned long long>(m),
         static_cast<unsigned long long>(src * n / m), gate);
  maskWrite(gCru, kSel24, (1u << 9) | (0x1fu << 4), ((div - 1) << 4));  // GPLL, divider
  wr(gCru, kSel25, static_cast<uint32_t>((n << 16) | m));
  dsb();
  maskWrite(gCru, kSel26, 3, 1);       // the fractional divider
  maskWrite(gCru, kSel28, 3u << 2, 0);  // MCLK out = the transmit clock
  maskWrite(gCru, kGate7, 0x00f3, 0);   // hclk / pclk of the audio root, hclk_i2s0_8ch, the transmit source, fraction and mclk: on
  const uint32_t grf = rd(gGrf, kGrfMclkOut);
  maskWrite(gGrf, kGrfMclkOut, 1, 0);  // on its way to the pin
  printf("evb10: audio: CRU sel24 %08x sel25 %08x sel26 %08x sel28 %08x gate7 %08x; SYS_GRF 318 %08x -> %08x\n", rd(gCru, kSel24), rd(gCru, kSel25),
         rd(gCru, kSel26), rd(gCru, kSel28), rd(gCru, kGate7), grf, rd(gGrf, kGrfMclkOut));
  return true;
}

void i2sPins() {
  iocSet(0x30, 0xff00, 0x1100, "GPIO1C_L: C2 = I2S0_MCLK, C3 = I2S0_SCLK");
  iocSet(0x34, 0xf0f0, 0x1010, "GPIO1C_H: C5 = I2S0_LRCK, C7 = I2S0_SDO0");
  iocSet(0x3c, 0x000f, 0x0002, "GPIO1D_H: D4 = I2S0_SDI0");
}

void dumpI2s(const char *when) {
  printf("evb10: audio: I2S0 %s: TXCR %08x RXCR %08x CKR %08x CLKDIV %08x DMACR %08x INTCR %08x INTSR %08x XFER %08x TXFIFO %08x RXFIFO %08x\n", when,
         rd(gI2s, kIsTxcr), rd(gI2s, kIsRxcr), rd(gI2s, kIsCkr), rd(gI2s, kIsClkdiv), rd(gI2s, kIsDmacr), rd(gI2s, kIsIntcr), rd(gI2s, kIsIntsr),
         rd(gI2s, kIsXfer), rd(gI2s, kIsTxFifo), rd(gI2s, kIsRxFifo));
}

void i2sSetup() {
  wr(gI2s, kIsXfer, 0);
  wr(gI2s, kIsClr, 3);  // clear the FIFOs; the bits drop by themselves
  for (unsigned i = 0; i < 1000 && (rd(gI2s, kIsClr) & 3); ++i) IODelay(1);
  // TXCR / RXCR: 16 bit valid data, two channels, I2S format, MSB first, no halfword transform. The path bits stay as they are.
  const uint32_t fields = 0x1fu | (3u << 15) | (1u << 14) | (1u << 12) | (1u << 11) | (3u << 9) | (3u << 7) | (3u << 5);
  wr(gI2s, kIsTxcr, (rd(gI2s, kIsTxcr) & ~fields) | 15u);
  wr(gI2s, kIsRxcr, (rd(gI2s, kIsRxcr) & ~fields) | 15u);
  // CKR: the controller is the master (MSS 0), transmit and receive share the transmit clocks (TRCM 1: "trcm-sync-tx-only" of the device tree),
  // normal polarities, 64 BCLK cycles per LRCK. The MCLK divider field stays as it is.
  const uint32_t ckr = (rd(gI2s, kIsCkr) & ~((3u << 28) | (1u << 27) | (7u << 24) | 0xffffu)) | (1u << 28) | (63u << 8) | 63u;
  wr(gI2s, kIsCkr, ckr);
  // CLKDIV: BCLK = MCLK / 4 (transmit in [7:0], receive in [15:8]): 12.288 MHz / 4 = 3.072 MHz = 64 x 48 kHz
  wr(gI2s, kIsClkdiv, (rd(gI2s, kIsClkdiv) & ~0xffffu) | (3u << 8) | 3u);
  wr(gI2s, kIsDmacr, 0);
  wr(gI2s, kIsIntcr, (1u << 2) | (1u << 18));  // no interrupts; clear the underrun and overrun flags
  dsb();
}

// One period of 1 kHz at 48 kHz is 48 samples; a quarter wave of it at 8192 (a quarter of full scale), mirrored.
int sineSample(uint32_t k) {
  static const int16_t q[13] = {0, 1069, 2120, 3135, 4096, 4987, 5793, 6499, 7094, 7568, 7913, 8122, 8192};
  k %= 48;
  const uint32_t a = k % 24;
  const int v = q[a <= 12 ? a : 24 - a];
  return k < 24 ? v : -v;
}

struct RxStats {
  uint32_t words = 0;
  int32_t min[2] = {32767, 32767}, max[2] = {-32768, -32768};
  int64_t sum[2] = {0, 0}, sumsq[2] = {0, 0};
  void add(uint32_t w) {
    ++words;
    for (int i = 0; i < 2; ++i) {
      const int32_t v = static_cast<int16_t>(i ? (w >> 16) : (w & 0xffff));
      if (v < min[i]) min[i] = v;
      if (v > max[i]) max[i] = v;
      sum[i] += v;
      sumsq[i] += static_cast<int64_t>(v) * v;
    }
  }
};

uint32_t isqrt(uint64_t v) {
  uint64_t r = 0, bit = 1ull << 62;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return static_cast<uint32_t>(r);
}

// The output amplifiers and the headphone detect are GPIOs (the card's hp-con-gpio = GPIO4_A7, spk-con-gpio = GPIO4_A2, both active high, and
// hp-det-gpio = GPIO1_C4, active low: low means a plug is in). GPIO v2 of the RK3588: DR at 0x00 and DDR at 0x08 (the _H half at +4 for pins 16..31, a write
// has the upper half-word as its mask), EXT_PORT at 0x70. pclk_gpio1 is CLKGATE_CON(16) bit 14, pclk_gpio4 CLKGATE_CON(17) bit 4 (1 = gated), and a read of a bank
// whose pclk is off hangs the bus, so the gates are opened first. The pins keep the function the firmware left them in (GPIO after a reset: the device tree
// gives these two no pin state).
constexpr uint64_t kGpio1Phys = 0xfec20000ULL, kGpio4Phys = 0xfec50000ULL;
volatile uint32_t *gGpio1, *gGpio4;
constexpr unsigned kHpCon = 7, kSpkCon = 2, kHpDet = 20;  // pin numbers in their banks (A0 = 0, B0 = 8, C0 = 16)

bool gpioOpen() {
  if (gGpio1 && gGpio4) return true;
  const uint32_t g16 = rd(gCru, 0x840), g17 = rd(gCru, 0x844);
  printf("evb10: audio: CLKGATE_CON(16) %08x CLKGATE_CON(17) %08x (pclk_gpio1 %s, pclk_gpio4 %s)\n", g16, g17, (g16 >> 14) & 1 ? "GATED" : "on",
         (g17 >> 4) & 1 ? "GATED" : "on");
  if ((g16 >> 14) & 1) wr(gCru, 0x840, 1u << (14 + 16));
  if ((g17 >> 4) & 1) wr(gCru, 0x844, 1u << (4 + 16));
  gGpio1 = reinterpret_cast<volatile uint32_t *>(ml_io_map(kGpio1Phys, 0x100));
  gGpio4 = reinterpret_cast<volatile uint32_t *>(ml_io_map(kGpio4Phys, 0x100));
  return gGpio1 && gGpio4;
}

void gpioOut(volatile uint32_t *bank, unsigned pin, bool high) {
  const uint32_t half = pin >= 16 ? 4 : 0, bit = pin & 15;
  wr(bank, 0x00 + half, (1u << (bit + 16)) | (high ? 1u << bit : 0u));
  wr(bank, 0x08 + half, (1u << (bit + 16)) | (1u << bit));
  dsb();
}

bool gpioIn(volatile uint32_t *bank, unsigned pin) { return (rd(bank, 0x70) >> pin) & 1; }

// 0: by the headphone detect, 1: headphone, 2: speaker, 3: both
unsigned gOutSelect = 0, gOutVolume = 28;

struct Route { bool headphone, speaker; };

Route chooseRoute() {
  Route r{false, true};
  if (gOutSelect == 0) {
    const bool plugged = !gpioIn(gGpio1, kHpDet);
    r.headphone = plugged;
    r.speaker = !plugged;
    printf("evb10: audio: headphone detect GPIO1_C4 reads %u: %s\n", gpioIn(gGpio1, kHpDet) ? 1u : 0u, plugged ? "a plug is in, the headphone output is used" : "no plug, the speaker output is used");
  } else {
    r.headphone = (gOutSelect & 1) != 0;
    r.speaker = (gOutSelect & 2) != 0;
  }
  return r;
}

void ampsSet(const Route &r, bool on) {
  gpioOut(gGpio4, kHpCon, on && r.headphone);
  gpioOut(gGpio4, kSpkCon, on && r.speaker);
}

// Codec for playback and capture (ES8388, slave): the order of the vendor driver's bias levels, then the powers, then the formats. The registers it
// changes are put back by codecIdle() from the dump at the start (the mixers, volumes and ADC settings stay as they are written here).
void codecActive() {
  const uint8_t c1 = gRegs[0x00];
  codecWrite(0x00, static_cast<uint8_t>((c1 & ~3u) | 3u | 4u));  // VMID 5k, reference on: charges the capacitors
  IOSleep(100);
  codecWrite(0x01, 0x50);                                           // overcurrent protection, analogue on
  codecWrite(0x00, static_cast<uint8_t>((c1 & ~3u) | 1u | 4u));  // VMID 2 x 50k
  codecWrite(0x02, 0x00);                                           // digital and clocks on
  codecWrite(0x03, 0x00);                                           // ADC, inputs and microphone bias on
  codecWrite(0x04, 0x3c);                                           // both DACs and the four outputs on
  codecWrite(0x08, 0x00);                                           // slave
  codecWrite(0x0c, 0x0c);                                           // ADC: I2S, 16 bit
  codecWrite(0x0d, 0x02);                                           // ADC: MCLK = 256 x fs
  codecWrite(0x17, 0x18);                                           // DAC: I2S, 16 bit
  codecWrite(0x18, 0x02);                                           // DAC: MCLK = 256 x fs
  codecWrite(0x2b, 0x80);                                           // DAC and ADC share LRCK
  codecWrite(0x19, 0x02);                                           // DAC not muted
  codecWrite(0x1a, 0x00);                                           // DAC digital volume 0 dB
  codecWrite(0x1b, 0x00);
  // Output mixers: the left DAC to the left mixer and the right DAC to the right one, no cross feed and no line-in bypass. (The register values a
  // Linux boot leaves, 0xb8 and 0x28, are the bypass at its largest.) The four output volumes are the same, 1.5 dB a step with 30 at 0 dB.
  codecWrite(0x26, 0x00);
  codecWrite(0x27, 0x80);
  codecWrite(0x28, 0x00);
  codecWrite(0x29, 0x00);
  codecWrite(0x2a, 0x80);
  for (uint8_t r = 0x2e; r <= 0x31; ++r) codecWrite(r, static_cast<uint8_t>(gOutVolume));
  // Capture: the board routes the microphones to LINPUT1/2 and RINPUT1/2 (the card's audio-routing); these are the values a Linux boot leaves for it:
  // +24 dB of microphone gain, differential inputs, no mute, high-pass filter on, ADC volume 0 dB.
  codecWrite(0x09, 0x88);
  codecWrite(0x0a, 0xf8);
  codecWrite(0x0b, 0x82);
  codecWrite(0x0e, 0x30);
  codecWrite(0x0f, 0x20);
  codecWrite(0x10, 0x00);
  codecWrite(0x11, 0x00);
  IOSleep(10);
}

void codecIdle() {
  codecWrite(0x04, gRegs[0x04]);
  codecWrite(0x03, gRegs[0x03]);
  codecWrite(0x02, gRegs[0x02]);
  codecWrite(0x01, gRegs[0x01]);
  codecWrite(0x00, gRegs[0x00]);
}

void tone(unsigned ms) {
  codecActive();
  const bool gpio = gpioOpen();
  Route route{false, false};
  if (gpio) {
    route = chooseRoute();
    IOSleep(30);  // the card's pre-power-on delay
    ampsSet(route, true);
    printf("evb10: audio: amplifier enables: headphone (GPIO4_A7) %s, speaker (GPIO4_A2) %s\n", route.headphone ? "on" : "off", route.speaker ? "on" : "off");
  } else {
    printf("evb10: audio: cannot map the GPIO banks: the amplifiers stay as the firmware left them\n");
  }
  i2sSetup();
  dumpI2s("set up");

  uint32_t phase = 0, pushed = 0;
  RxStats rx;
  auto word = [&phase]() {
    const uint32_t s = static_cast<uint32_t>(sineSample(phase++)) & 0xffff;
    return (s << 16) | s;
  };
  // Fill the FIFO before the clocks start, then start transmit and receive together.
  for (unsigned i = 0; i < 24; ++i) wr(gI2s, kIsTxdr, word());
  pushed = 24;
  const uint64_t began = nowUs();
  wr(gI2s, kIsXfer, 3);
  dsb();
  bool stalled = false;
  uint32_t maxLevel = 0;
  for (;;) {
    const uint64_t now = nowUs();
    if (now - began > static_cast<uint64_t>(ms) * 1000) break;
    uint32_t level = rd(gI2s, kIsTxFifo) & 0x3f;
    while (level < 28) {
      wr(gI2s, kIsTxdr, word());
      ++pushed;
      ++level;
    }
    if (level > maxLevel) maxLevel = level;
    for (uint32_t r = rd(gI2s, kIsRxFifo) & 0x3f; r; --r) rx.add(rd(gI2s, kIsRxdr));
    if (now - began > 200000 && pushed < 200) {
      stalled = true;
      break;
    }
    IODelay(30);
  }
  const uint64_t took = nowUs() - began;
  const uint32_t intsr = rd(gI2s, kIsIntsr);
  wr(gI2s, kIsXfer, 0);
  dsb();
  dumpI2s("after");
  if (gpio) ampsSet(route, false);
  codecIdle();

  printf("evb10: audio: transmit: %u words in %llu us = %llu words/s%s (INTSR %08x, FIFO level at most %u)\n", pushed, static_cast<unsigned long long>(took),
         took ? static_cast<unsigned long long>(pushed * 1000000ull / took) : 0ull, stalled ? ", the FIFO did not drain" : "", intsr, maxLevel);
  printf("evb10: audio: receive: %u words, %llu words/s\n", rx.words, took ? static_cast<unsigned long long>(rx.words * 1000000ull / took) : 0ull);
  if (rx.words) {
    for (int i = 0; i < 2; ++i) {
      const int64_t mean = rx.sum[i] / rx.words;
      const int64_t var = rx.sumsq[i] / rx.words - mean * mean;
      printf("evb10: audio: receive %s half: min %d max %d mean %lld rms-about-mean %u\n", i ? "high" : "low", rx.min[i], rx.max[i],
             static_cast<long long>(mean), isqrt(var > 0 ? static_cast<uint64_t>(var) : 0));
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------- checkpoint 3: the service

// What the engine below offers the user client (defined after the classes).
IOReturn audioRequest(uint32_t selector);
IOMemoryDescriptor *audioMemory(uint32_t type);
void audioServe();

}  // namespace

// A service with no provider matching: audioServe() makes it by hand, attaches it to the resource service and registers it, so that
// IOServiceGetMatchingService(IOServiceMatching("Evb10Audio")) in the audio server finds it. The audio server's sandbox profile names no class of this
// driver; the service lists the processes that may open it in NativeSandboxGrantProcess (the kernel's native_sandbox_grant, UniXNU's
// zzw-native-adreno-sandbox-grant) in place of the sandbox's verdict. Each IOServiceOpen makes one Evb10AudioClient.
class Evb10Audio : public IOService {
  OSDeclareDefaultStructors(Evb10Audio)
 public:
  using IOService::newUserClient;
  IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) override;
};
OSDefineMetaClassAndStructors(Evb10Audio, IOService)

class Evb10AudioClient : public IOUserClient {
  OSDeclareDefaultStructors(Evb10AudioClient)
  char ownerName[17] = {};

 public:
  using IOUserClient::initWithTask;
  using IOUserClient::clientMemoryForType;
  bool initWithTask(task_t owningTask, void *securityToken, UInt32 type) override;
  IOReturn clientClose() override;
  IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments, IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) override;
};
OSDefineMetaClassAndStructors(Evb10AudioClient, IOUserClient)

extern "C" void proc_selfname(char *buf, int size);

IOReturn Evb10Audio::newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) {
  if (!handler) return kIOReturnBadArgument;
  Evb10AudioClient *client = OSTypeAlloc(Evb10AudioClient);
  if (!client) return kIOReturnNoMemory;
  bool attached = false;
  if (!client->initWithTask(owningTask, securityID, type) || !(attached = client->attach(this)) || !client->start(this)) {
    if (attached) client->detach(this);
    client->release();
    return kIOReturnError;
  }
  *handler = client;
  return kIOReturnSuccess;
}

bool Evb10AudioClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type) {
  if (!IOUserClient::initWithTask(owningTask, securityToken, type)) return false;
  proc_selfname(ownerName, sizeof ownerName);
  printf("evb10: audio: %s opens the service\n", ownerName);
  return true;
}

IOReturn Evb10AudioClient::clientClose() {
  // The plug-in's process went away or closed: nothing plays from its rings any more.
  audioRequest(Evb10AudioStopOutput);
  audioRequest(Evb10AudioStopInput);
  printf("evb10: audio: %s closes the service\n", ownerName);
  if (!isInactive()) (void)terminate();
  return kIOReturnSuccess;
}

IOReturn Evb10AudioClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
  if (!options || !memory) return kIOReturnBadArgument;
  IOMemoryDescriptor *m = audioMemory(type);
  if (!m) return kIOReturnBadArgument;
  *options = 0;
  *memory = m;  // retained: the caller consumes the reference
  return kIOReturnSuccess;
}

IOReturn Evb10AudioClient::externalMethod(uint32_t selector, IOExternalMethodArguments *arguments, IOExternalMethodDispatch *, OSObject *, void *) {
  if (selector > Evb10AudioStopInput || !arguments || arguments->scalarInputCount || arguments->structureInputSize) return kIOReturnBadArgument;
  return audioRequest(selector);
}

namespace {

// ---- the engine

struct Engine {
  IOBufferMemoryDescriptor *control = nullptr, *outRing = nullptr, *inRing = nullptr;
  Evb10AudioControl *ctl = nullptr;
  int16_t *out = nullptr, *in = nullptr;
  IOLock *lock = nullptr;
  volatile uint32_t want = 0;      // Evb10Audio*Running bits: what the plug-in asked for
  volatile uint32_t hardware = 0;  // 1 while the I2S clocks run and the thread keeps the FIFOs
  volatile uint32_t serving = 0;   // the control page and the rings exist
  uint64_t n = 0, nIn = 0;         // the next playback sample time to take, the next capture sample time to store
  Route route{false, false};
  unsigned lingerMs = 3000;
  // statistics of this run of the hardware
  uint32_t ticks = 0, underruns = 0, overruns = 0, skippedEvents = 0, level[4] = {0, 0, 0, 0};
  uint32_t gapBucket[6] = {0, 0, 0, 0, 0, 0};
  uint64_t pushed = 0, drained = 0;
  IOService *service = nullptr;
} gE;

constexpr uint32_t kFifoFrames = 28;  // how far ahead of the wire the transmit FIFO is filled (its depth is 32)
constexpr uint32_t kFifoDepth = 32;

uint64_t nowMs() {
  uint64_t ns;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}

// floor(num * 2^32 / den) without a 128-bit division (the kernel has no __udivti3); den < 2^63.
uint64_t divQ32(uint64_t num, uint64_t den) {
  uint64_t whole = num / den, rest = num % den, fraction = 0;
  for (int i = 0; i < 32; ++i) {
    rest <<= 1;
    fraction <<= 1;
    if (rest >= den) {
      rest -= den;
      fraction |= 1;
    }
  }
  return (whole << 32) | fraction;
}

// The frame of the shared timeline that is on the wire at a host time.
uint64_t framesAt(uint64_t host) {
  const uint64_t anchor = gE.ctl->anchor_host;
  return host > anchor ? divQ32(host - anchor, gE.ctl->ticks_per_frame_q32) : 0;
}

bool setupShared() {
  const IOOptionBits shared = static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryKernelUserShared);
  const size_t outBytes = static_cast<size_t>(EVB10_AUDIO_RING_FRAMES) * EVB10_AUDIO_OUT_CHANNELS * 2;
  const size_t inBytes = static_cast<size_t>(EVB10_AUDIO_RING_FRAMES) * EVB10_AUDIO_IN_CHANNELS * 2;
  gE.control = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared, 4096, 4096);
  gE.outRing = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared, outBytes, 4096);
  gE.inRing = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared, inBytes, 4096);
  if (!gE.control || !gE.outRing || !gE.inRing) return false;
  gE.ctl = static_cast<Evb10AudioControl *>(gE.control->getBytesNoCopy());
  gE.out = static_cast<int16_t *>(gE.outRing->getBytesNoCopy());
  gE.in = static_cast<int16_t *>(gE.inRing->getBytesNoCopy());
  memset(gE.ctl, 0, 4096);
  memset(gE.out, 0, outBytes);
  memset(gE.in, 0, inBytes);
  mach_timebase_info_data_t tb;
  clock_timebase_info(&tb);
  Evb10AudioControl *c = gE.ctl;
  c->rate = EVB10_AUDIO_RATE;
  c->out_channels = EVB10_AUDIO_OUT_CHANNELS;
  c->in_channels = EVB10_AUDIO_IN_CHANNELS;
  c->period_frames = EVB10_AUDIO_PERIOD_FRAMES;
  c->ring_frames = EVB10_AUDIO_RING_FRAMES;
  c->lead_frames = 64;  // the service takes a frame at most 28 ahead of the wire; the rest is the codec's own delay
  c->fifo_frames = kFifoFrames;
  // host ticks per frame in 32.32 fixed point from the timebase (125/3 for the 24 MHz timer: 500 ticks per frame)
  c->ticks_per_frame_q32 = divQ32(1000000000ULL * tb.denom, static_cast<uint64_t>(EVB10_AUDIO_RATE) * tb.numer);
  c->anchor_host = mach_absolute_time();
  c->version = EVB10_AUDIO_VERSION;
  printf("evb10: audio: timebase %u/%u, %llu.%03llu host ticks per frame, rings of %u frames (%zu + %zu bytes)\n", tb.numer, tb.denom,
         static_cast<unsigned long long>(c->ticks_per_frame_q32 >> 32), static_cast<unsigned long long>(((c->ticks_per_frame_q32 & 0xffffffffULL) * 1000) >> 32),
         EVB10_AUDIO_RING_FRAMES, outBytes, inBytes);
  __atomic_store_n(&c->magic, EVB10_AUDIO_MAGIC, __ATOMIC_RELEASE);
  return true;
}

void resetStats() {
  gE.ticks = gE.underruns = gE.overruns = gE.skippedEvents = 0;
  for (auto &v : gE.level) v = 0;
  for (auto &v : gE.gapBucket) v = 0;
  gE.pushed = gE.drained = 0;
  gE.ctl->out_late = 0;
  gE.ctl->in_late = 0;
  gE.ctl->out_skipped_frames = 0;
  gE.ctl->out_peak = 0;
  gE.ctl->in_peak = 0;
  gE.ctl->wakeups = 0;
  gE.ctl->max_gap_us = 0;
}

bool hardwareStart() {
  resetStats();
  codecActive();
  gE.route = chooseRoute();
  IOSleep(30);  // the card's pre-power-on delay
  ampsSet(gE.route, true);
  printf("evb10: audio: hardware on: headphone amplifier %s, speaker amplifier %s, output volume %u\n", gE.route.headphone ? "on" : "off",
         gE.route.speaker ? "on" : "off", gOutVolume);
  i2sSetup();
  memset(gE.in, 0, static_cast<size_t>(EVB10_AUDIO_RING_FRAMES) * EVB10_AUDIO_IN_CHANNELS * 2);
  for (unsigned i = 0; i < 24; ++i) wr(gI2s, kIsTxdr, 0);
  const uint64_t t0 = mach_absolute_time();
  wr(gI2s, kIsXfer, 3);
  dsb();
  gE.n = framesAt(t0) + 24;
  gE.nIn = framesAt(t0);
  gE.ctl->out_frames = gE.n;
  gE.ctl->in_frames = gE.nIn;
  __atomic_store_n(&gE.ctl->in_written, gE.nIn, __ATOMIC_RELEASE);
  return true;
}

// Two lines: the kernel's printf cuts a line at about 500 characters.
void reportRun(const char *what) {
  Evb10AudioControl *c = gE.ctl;
  printf("evb10: audio: %s: %u ticks, longest gap %u us, gaps <300/<600/<1000/<2000/<5000/more us: %u %u %u %u %u %u\n", what, gE.ticks, c->max_gap_us, gE.gapBucket[0],
         gE.gapBucket[1], gE.gapBucket[2], gE.gapBucket[3], gE.gapBucket[4], gE.gapBucket[5]);
  printf("evb10: audio: %s: transmit %llu frames, receive %llu frames; underruns %u, overruns %u, FIFO ran dry %u times (%u frames skipped); FIFO level at a tick %u..%u; "
         "peaks out %u in %u\n",
         what, static_cast<unsigned long long>(gE.pushed), static_cast<unsigned long long>(gE.drained), gE.underruns, gE.overruns, gE.skippedEvents, c->out_skipped_frames,
         gE.level[0], gE.level[1], c->out_peak, c->in_peak);
}

void hardwareStop() {
  ampsSet(gE.route, false);
  IOSleep(40);  // the card's post-power-down delay
  wr(gI2s, kIsXfer, 0);
  dsb();
  reportRun("hardware off");
  codecIdle();
}

// The plug-in's record of what the audio server asked for and the service does not answer (the diagnostics half of the control page): printed once
// per change, so the kernel log names it without the audio server's own log.
void reportDiag() {
  static uint32_t seenProps, seenTrace, seenLoads, seenSets;
  const Evb10AudioDiag *d = reinterpret_cast<const Evb10AudioDiag *>(reinterpret_cast<const uint8_t *>(gE.ctl) + EVB10_AUDIO_DIAG_OFFSET);
  const Evb10AudioTrace *t = reinterpret_cast<const Evb10AudioTrace *>(reinterpret_cast<const uint8_t *>(gE.ctl) + EVB10_AUDIO_TRACE_OFFSET);
  if (d->loads != seenLoads) {
    seenLoads = d->loads;
    printf("evb10: audio: the audio server loaded the plug-in (%u time(s))\n", seenLoads);
  }
  for (; seenProps < d->count && seenProps < EVB10_AUDIO_DIAG_SLOTS; ++seenProps) {
    const uint32_t v = d->selector[seenProps], w = d->where[seenProps];
    printf("evb10: audio: [props] unanswered property '%c%c%c%c' of object %u, scope '%c'\n", static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8),
           static_cast<char>(v), w >> 24, static_cast<char>(w & 0xff));
  }
  for (; seenTrace < t->count && seenTrace < EVB10_AUDIO_TRACE_SLOTS; ++seenTrace) {
    const uint32_t v = t->entry[seenTrace][0], w = t->entry[seenTrace][1];
    printf("evb10: audio: [trace %u] selector '%c%c%c%c' (%08x) object %u scope '%c' kind %u%s\n", seenTrace, static_cast<char>(v >> 24), static_cast<char>(v >> 16),
           static_cast<char>(v >> 8), static_cast<char>(v), v, w >> 24, static_cast<char>((w >> 16) & 0xff), w & 1 ? 1u : (w & 4) ? 4u : (w & 8) ? 8u : 0u,
           (w & 2) ? ", failed" : "");
  }
  if (d->volume_sets != seenSets) {
    seenSets = d->volume_sets;
    printf("evb10: audio: the audio server set the output volume to %u/1000 (%u times)\n", d->volume_milli, seenSets);
  }
}

void tick(uint64_t &last) {
  Evb10AudioControl *c = gE.ctl;
  const uint64_t now = mach_absolute_time();
  const uint64_t wire = framesAt(now);
  // How long since the last tick.
  if (last) {
    uint64_t ns;
    absolutetime_to_nanoseconds(now - last, &ns);
    const uint32_t us = static_cast<uint32_t>(ns / 1000);
    if (us > c->max_gap_us) c->max_gap_us = us;
    ++gE.gapBucket[us < 300 ? 0 : us < 600 ? 1 : us < 1000 ? 2 : us < 2000 ? 3 : us < 5000 ? 4 : 5];
  }
  last = now;
  ++gE.ticks;
  c->wakeups = gE.ticks;

  const uint32_t sr = rd(gI2s, kIsIntsr);
  if (sr & (1u << 1)) {
    ++gE.underruns;
    c->out_late = c->out_late + 1;
    wr(gI2s, kIsIntcr, 1u << 2);  // TXUIC
  }
  if (sr & (1u << 17)) {
    ++gE.overruns;
    c->in_late = c->in_late + 1;
    wr(gI2s, kIsIntcr, 1u << 18);  // RXOIC
  }

  // Playback: the frames from the next one to take up to kFifoFrames ahead of the wire.
  uint32_t fill = rd(gI2s, kIsTxFifo) & 0x3f;
  if (gE.ticks > 4) {
    if (fill < gE.level[0] || !gE.level[0]) gE.level[0] = fill;
    if (fill > gE.level[1]) gE.level[1] = fill;
  }
  const uint64_t earliest = wire + fill;  // the wire slot a word pushed now would reach
  if (gE.n + 2 < earliest) {
    // The FIFO ran dry and the wire has passed these frames: skip them (cleared, so they do not play a turn later) to keep the ring's timeline.
    const uint64_t skip = earliest - gE.n;
    const uint64_t clear = skip < EVB10_AUDIO_RING_FRAMES ? skip : EVB10_AUDIO_RING_FRAMES;
    for (uint64_t i = 0; i < clear; ++i) {
      int16_t *f = gE.out + 2 * ((gE.n + i) % EVB10_AUDIO_RING_FRAMES);
      f[0] = f[1] = 0;
    }
    gE.n = earliest;
    ++gE.skippedEvents;
    c->out_skipped_frames += static_cast<uint32_t>(skip);
  }
  const uint64_t target = wire + kFifoFrames;
  uint32_t peak = 0;
  while (gE.n < target && fill < kFifoDepth) {
    int16_t *f = gE.out + 2 * (gE.n % EVB10_AUDIO_RING_FRAMES);
    const int32_t l = f[0], r = f[1];
    f[0] = f[1] = 0;
    const uint32_t al = static_cast<uint32_t>(l < 0 ? -l : l), ar = static_cast<uint32_t>(r < 0 ? -r : r);
    if (al > peak) peak = al;
    if (ar > peak) peak = ar;
    wr(gI2s, kIsTxdr, (static_cast<uint32_t>(static_cast<uint16_t>(r)) << 16) | static_cast<uint16_t>(l));
    ++gE.n;
    ++fill;
    ++gE.pushed;
  }
  if (peak > c->out_peak) c->out_peak = peak;
  c->out_frames = gE.n;

  // Capture: both halves of a word are the two channels; the plug-in has one input channel, the mean of them.
  uint32_t inPeak = 0;
  for (uint32_t r = rd(gI2s, kIsRxFifo) & 0x3f; r; --r) {
    const uint32_t w = rd(gI2s, kIsRxdr);
    const int32_t l = static_cast<int16_t>(w & 0xffff), rr = static_cast<int16_t>(w >> 16);
    const int32_t m = (l + rr) / 2;
    gE.in[gE.nIn % EVB10_AUDIO_RING_FRAMES] = static_cast<int16_t>(m);
    const uint32_t am = static_cast<uint32_t>(m < 0 ? -m : m);
    if (am > inPeak) inPeak = am;
    ++gE.nIn;
    ++gE.drained;
  }
  if (inPeak > c->in_peak) c->in_peak = inPeak;
  c->in_frames = gE.nIn;
  __atomic_store_n(&c->in_written, gE.nIn, __ATOMIC_RELEASE);
}

void engineLoop() {
  for (;;) {
    IOLockLock(gE.lock);
    while (!gE.want) IOLockSleep(gE.lock, const_cast<uint32_t *>(&gE.want), THREAD_UNINT);
    IOLockUnlock(gE.lock);
    if (!hardwareStart()) {
      printf("evb10: audio: the hardware did not start\n");
      IOSleep(1000);
      continue;
    }
    __atomic_store_n(&gE.hardware, 1u, __ATOMIC_RELEASE);
    __atomic_or_fetch(&gE.ctl->state, static_cast<uint32_t>(Evb10AudioHardwareOn), __ATOMIC_ACQ_REL);
    uint64_t last = 0, lingerUntil = 0;
    for (;;) {
      tick(last);
      const uint32_t want = __atomic_load_n(&gE.want, __ATOMIC_ACQUIRE);
      if (want) {
        lingerUntil = 0;
      } else if (!lingerUntil) {
        lingerUntil = nowMs() + gE.lingerMs;
      } else if (nowMs() >= lingerUntil) {
        break;
      }
      // Nothing is printed from here while the hardware runs: a kernel printf at 1.5 Mbaud takes about 0.7 ms a line, more than the FIFO holds
      // (the first version printed its statistics and the plug-in's trace from this thread and ran dry at every report, once for 68 ms).
      delay_for_interval_with_leeway(200, 0, 1000);  // 200 us: the FIFO holds 667 us
    }
    __atomic_store_n(&gE.hardware, 0u, __ATOMIC_RELEASE);
    __atomic_and_fetch(&gE.ctl->state, ~static_cast<uint32_t>(Evb10AudioHardwareOn), __ATOMIC_ACQ_REL);
    hardwareStop();
  }
}

// What the plug-in's own counters say (the diagnostics half of the control page), once per 5 s while it is writing.
void reportPlugin() {
  static uint32_t seenWrites;
  const Evb10AudioDiag *d = reinterpret_cast<const Evb10AudioDiag *>(reinterpret_cast<const uint8_t *>(gE.ctl) + EVB10_AUDIO_DIAG_OFFSET);
  if (d->writes == seenWrites) return;
  seenWrites = d->writes;
  printf("evb10: audio: plug-in: %u writes of %u frames (%u frames in all); write-ahead %d..%d frames, read-behind %d..%d; mix peak %u, input misses %u; "
         "cycle intervals <15/<25/<40/<80/<160/more ms: %u %u %u %u %u %u\n",
         d->writes, d->io_frames, d->frames_total, d->ahead_min, d->ahead_max, d->behind_min, d->behind_max, d->mix_peak, d->in_misses, d->interval[0], d->interval[1],
         d->interval[2], d->interval[3], d->interval[4], d->interval[5]);
  printf("evb10: audio: plug-in: overlaps %u (most %u frames), skipped stretches %u (%u frames); IO thread priority %d (base %d, lowest %d), time constraint %d "
         "(period %u); host clock %u Hz (lookup %d)\n",
         d->overlaps, d->overlap_max, d->gaps, d->gap_frames, d->io_pri, d->io_base, d->io_pri_min, d->io_tc, d->io_period, d->host_clock, d->host_clock_result);
}

// The log's thread: the engine thread does not print while the hardware runs, this one does (at the kernel's default priority).
void reporterMain(void *, wait_result_t) {
  uint64_t lastReport = nowMs();
  for (;;) {
    IOSleep(200);
    reportDiag();
    const uint64_t ms = nowMs();
    if (ms - lastReport >= 5000) {
      lastReport = ms;
      if (__atomic_load_n(&gE.hardware, __ATOMIC_ACQUIRE)) reportRun("running");
      reportPlugin();
    }
  }
}

IOMemoryDescriptor *audioMemory(uint32_t type) {
  if (!__atomic_load_n(&gE.serving, __ATOMIC_ACQUIRE)) return nullptr;
  IOMemoryDescriptor *m = type == Evb10AudioMemoryControl ? gE.control : type == Evb10AudioMemoryOutput ? gE.outRing : type == Evb10AudioMemoryInput ? gE.inRing : nullptr;
  if (m) m->retain();
  return m;
}

// StartOutput / StartInput return once the hardware runs: the plug-in places its timeline from the moment the call returns.
IOReturn audioRequest(uint32_t selector) {
  if (!__atomic_load_n(&gE.serving, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
  IOLockLock(gE.lock);
  switch (selector) {
    case Evb10AudioStartOutput: gE.want |= Evb10AudioOutputRunning; break;
    case Evb10AudioStopOutput: gE.want &= ~static_cast<uint32_t>(Evb10AudioOutputRunning); break;
    case Evb10AudioStartInput: gE.want |= Evb10AudioInputRunning; break;
    case Evb10AudioStopInput: gE.want &= ~static_cast<uint32_t>(Evb10AudioInputRunning); break;
    default: IOLockUnlock(gE.lock); return kIOReturnBadArgument;
  }
  const uint32_t want = gE.want;
  gE.ctl->state = (gE.ctl->state & Evb10AudioHardwareOn) | want;
  IOLockWakeup(gE.lock, const_cast<uint32_t *>(&gE.want), false);
  IOLockUnlock(gE.lock);
  static unsigned logged;
  if (logged++ < 64) printf("evb10: audio: request %u (%s), wanted now %#x\n", selector, selector == 0 ? "start output" : selector == 1 ? "stop output" : selector == 2 ? "start input" : "stop input", want);
  if (selector == Evb10AudioStartOutput || selector == Evb10AudioStartInput) {
    for (unsigned i = 0; i < 400 && !__atomic_load_n(&gE.hardware, __ATOMIC_ACQUIRE); ++i) IOSleep(5);
    if (!__atomic_load_n(&gE.hardware, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
  }
  return kIOReturnSuccess;
}

// evb10-audio-selftest=N: the service drives itself for N seconds, writing a 480 Hz triangle into the playback ring as the plug-in would (2400 frames,
// 50 ms, ahead of the play position, added to what the slot holds) and starting both directions the same way. It checks the whole engine without the audio
// server: the FIFO accounting, the peaks and the capture ring show in the log.
void selftestMain(void *, wait_result_t) {
  unsigned secs = 0;
  PE_parse_boot_argn("evb10-audio-selftest", &secs, sizeof secs);
  if (secs < 1 || secs > 120) secs = 10;
  IOSleep(2000);
  Evb10AudioControl *c = gE.ctl;
  printf("evb10: audio: self-test: %u s of a 480 Hz triangle (amplitude 8000) through the rings\n", secs);
  if (audioRequest(Evb10AudioStartOutput) != kIOReturnSuccess) {
    printf("evb10: audio: self-test: the output did not start\n");
    thread_terminate(current_thread());
    return;
  }
  audioRequest(Evb10AudioStartInput);
  uint64_t w = framesAt(mach_absolute_time()) + 2400;
  const uint64_t end = nowMs() + static_cast<uint64_t>(secs) * 1000;
  while (nowMs() < end) {
    const uint64_t target = framesAt(mach_absolute_time()) + 2400 + 480;
    for (; w < target; ++w) {
      const int32_t phase = static_cast<int32_t>(w % 100);
      const int32_t v = phase < 50 ? -8000 + 320 * phase : 8000 - 320 * (phase - 50);
      int16_t *f = gE.out + 2 * (w % EVB10_AUDIO_RING_FRAMES);
      for (int ch = 0; ch < 2; ++ch) {
        const int32_t sum = f[ch] + v;
        f[ch] = static_cast<int16_t>(sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum);
      }
    }
    c->out_written = w;
    c->out_written_host = mach_absolute_time();
    IOSleep(10);
  }
  audioRequest(Evb10AudioStopOutput);
  audioRequest(Evb10AudioStopInput);
  printf("evb10: audio: self-test done: %llu frames written into the ring\n", static_cast<unsigned long long>(w));
  thread_terminate(current_thread());
}

void audioServe() {
  unsigned linger = 3000;
  if (PE_parse_boot_argn("evb10-audio-linger", &linger, sizeof linger) && linger > 600000) linger = 3000;
  gE.lingerMs = linger;
  if (!gpioOpen()) {
    printf("evb10: audio: cannot map the GPIO banks: no service\n");
    return;
  }
  gE.lock = IOLockAlloc();
  if (!gE.lock || !setupShared()) {
    printf("evb10: audio: no memory for the rings: no service\n");
    return;
  }
  gE.service = OSTypeAlloc(Evb10Audio);
  if (!gE.service || !gE.service->init() || !gE.service->attach(IOService::getResourceService()) || !gE.service->start(IOService::getResourceService())) {
    printf("evb10: audio: could not make the service\n");
    return;
  }
  char grant[128] = "audiomxd,mediaserverd";
  PE_parse_boot_argn("evb10-audio-grant", grant, sizeof grant);
  gE.service->setProperty("NativeSandboxGrantProcess", grant);
  __atomic_store_n(&gE.serving, 1u, __ATOMIC_RELEASE);
  gE.service->registerService();
  printf("evb10: audio: service \"%s\" registered, sandbox grant: %s; output %s, volume %u, hardware stays on %u ms after the last stop\n", EVB10_AUDIO_CLASS, grant,
         gOutSelect == 0 ? "by the headphone detect" : gOutSelect == 1 ? "headphone" : gOutSelect == 2 ? "speaker" : "both", gOutVolume, gE.lingerMs);
  thread_t reporter = nullptr;
  if (kernel_thread_start(reporterMain, nullptr, &reporter) == KERN_SUCCESS) thread_deallocate(reporter);
  unsigned selftest = 0;
  if (PE_parse_boot_argn("evb10-audio-selftest", &selftest, sizeof selftest) && selftest) {
    thread_t t = nullptr;
    if (kernel_thread_start(selftestMain, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
  }
  engineLoop();
}

// ---------------------------------------------------------------------------------------------------------------- one-shot

void run(unsigned level) {
  gI2c = reinterpret_cast<volatile uint32_t *>(ml_io_map(kI2c7, 0x1000));
  gIoc = reinterpret_cast<volatile uint32_t *>(ml_io_map(kBusIoc, 0x1000));
  gCru = reinterpret_cast<volatile uint32_t *>(ml_io_map(kCru, 0x1000));
  if (!gI2c || !gIoc || !gCru) {
    printf("evb10: audio: cannot map the controllers\n");
    return;
  }
  unsigned addr = 0;
  if (PE_parse_boot_argn("evb10-audio-addr", &addr, sizeof addr) && addr >= 0x08 && addr < 0x78) gAddr = static_cast<uint8_t>(addr);
  const uint32_t version = (rd(gI2c, kCon) >> 16) & 0x1ff;
  const uint32_t rate = clockRate();
  i2cPins();
  gBus.disable();
  unsigned khz = 100;
  if (PE_parse_boot_argn("evb10-audio-khz", &khz, sizeof khz) && (khz < 100 || khz > 400)) khz = 100;
  printf("evb10: audio: I2C7 version %u, clock %u Hz, SCL %u kHz, ", version, rate, khz);
  if (!gBus.setSpeed(khz * 1000, rate)) {
    printf("cannot set %u kHz\n", khz);
    return;
  }
  printf("CLKDIV %08x\n", rd(gI2c, kClkDiv));

  unsigned found = 0;
  char line[160];
  unsigned at = 0;
  for (unsigned a = 0x08; a < 0x78; ++a) {
    if (gBus.probe(static_cast<uint8_t>(a))) {
      if (at + 6 < sizeof line) at += static_cast<unsigned>(snprintf(line + at, sizeof line - at, " 0x%02x", a));
      ++found;
    }
  }
  printf("evb10: audio: I2C7 scan: %u device(s) answer:%s\n", found, found ? line : " none");
  if (!gBus.probe(gAddr)) {
    printf("evb10: audio: nothing answers at 0x%02x\n", gAddr);
    return;
  }
  printf("evb10: audio: 0x%02x answers\n", gAddr);
  dumpRegisters();
  writeCheck();
  if (level < 2) return;

  gPmu = reinterpret_cast<volatile uint32_t *>(ml_io_map(kPmuPhys, 0x400));
  gGrf = reinterpret_cast<volatile uint32_t *>(ml_io_map(kGrfPhys, 0x1000));
  if (!gPmu || !gGrf) {
    printf("evb10: audio: cannot map the PMU or the GRF\n");
    return;
  }
  if (!audioDomainOn()) {
    printf("evb10: audio: the audio power domain is off: the I2S controller is not touched (checkpoint 2 stops here)\n");
    return;
  }
  if (!startMclk()) return;
  i2sPins();
  gI2s = reinterpret_cast<volatile uint32_t *>(ml_io_map(kI2s0Phys, 0x1000));
  if (!gI2s) {
    printf("evb10: audio: cannot map I2S0\n");
    return;
  }
  dumpI2s("found");
  unsigned out = 0, volume = 28;
  if (PE_parse_boot_argn("evb10-audio-out", &out, sizeof out) && out <= 3) gOutSelect = out;
  if (PE_parse_boot_argn("evb10-audio-vol", &volume, sizeof volume) && volume <= 36) gOutVolume = volume;
  if (level >= 4) {
    audioServe();   // does not return
    return;
  }
  unsigned ms = 2000;
  if (PE_parse_boot_argn("evb10-audio-ms", &ms, sizeof ms) && (ms < 100 || ms > 10000)) ms = 2000;
  tone(ms);
}

unsigned gLevel;

void workerMain(void *, wait_result_t) {
  run(gLevel);
  thread_terminate(current_thread());
}

}  // namespace

// Called by the display probe once the HID services are up (package/LegacyDisplayProbe.cpp).
extern "C" void evb10_audio_start(void) {
  static bool started;
  unsigned on = 0;
  if (!PE_parse_boot_argn("evb10-audio", &on, sizeof on) || on < 1 || on > 4 || started) return;
  started = true;
  gLevel = on;
  thread_t t = nullptr;
  printf("evb10: audio: starting (level %u)\n", on);
  // The service's thread keeps a 667 us FIFO full: it runs at the highest priority of the kernel's own band.
  const kern_return_t r = on >= 4 ? kernel_thread_start_priority(workerMain, nullptr, 95, &t) : kernel_thread_start(workerMain, nullptr, &t);
  if (r == KERN_SUCCESS) {
    thread_set_thread_name(t, "evb10-audio");
    thread_deallocate(t);
  } else {
    printf("evb10: audio: no worker thread\n");
  }
}
