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
// The I2C master is the polled one of U-Boot's rk_i2c.c (GPL-2.0+, Rockchip) rewritten for the kernel, the same as package/Evb10Touch has for
// I2C5; the two are separate copies because the touch driver's state is file-local and it is in use. The clock and pin numbers are those of
// the vendor kernel's clk-rk3588.c, rockchip_i2s_tdm.c and the board's device tree.
//
// Boot-args: evb10-audio=N       1: checkpoint 1; 2: checkpoint 1 and 2
//            evb10-audio-addr=N  the codec's 7 bit address (default 0x11)
//            evb10-audio-khz=N   the SCL rate, 100..400 (default 100)
//            evb10-audio-ms=N    how long the tone and the capture run (default 2000, 100..10000)
#include <IOKit/IOLib.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdint.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
// (kern/thread.h is not included: it changes how IOLib.h's typed allocators compile under -Werror)
extern "C" kern_return_t kernel_thread_start(thread_continue_t continuation, void *parameter, thread_t *new_thread);
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

// Codec for playback and capture (ES8388, slave): the order of the vendor driver's bias levels, then the powers, then the formats. The registers it
// changes are put back by codecIdle() from the dump at the start.
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
  codecWrite(0x1a, 0x00);
  codecWrite(0x1b, 0x00);
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
  if (!PE_parse_boot_argn("evb10-audio", &on, sizeof on) || on < 1 || on > 2 || started) return;
  started = true;
  gLevel = on;
  thread_t t = nullptr;
  printf("evb10: audio: starting (level %u)\n", on);
  if (kernel_thread_start(workerMain, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
  else printf("evb10: audio: no worker thread\n");
}
