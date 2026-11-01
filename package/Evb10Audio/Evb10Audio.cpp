// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Evb10Audio: the audio codec of the Rockchip RK3588 EVB10 (docs/audio.md).
//
// Checkpoint 1 (this file so far): the kernel talks to the codec. The board's `es8388-sound` card has an Everest ES8388 at address 0x11 of
// I2C7 (0xfec90000, pins GPIO1_D0 = SCL / GPIO1_D1 = SDA, function 9, the "i2c7m0" routing of the board's device tree), with the I2S0
// controller (0xfe470000) as its digital side. Nothing here touches I2S0 yet: it sits in the audio power domain, and a read of a block
// whose domain is off would hang the bus, so that comes with the code that powers the domain.
//
// What it does, once, from a thread call: opens the I2C7 clock gates if the firmware left them closed, routes the two pins, scans the bus,
// dumps the codec's registers 0x00..0x34 and checks that a write reads back (LOUT1VOL, restored afterwards). The result is in the log
// (evb10: audio: ...); an acknowledged address and a register that holds a value is what "the codec answers" means here.
//
// The I2C master is the polled one of U-Boot's rk_i2c.c (GPL-2.0+, Rockchip) rewritten for the kernel, the same as package/Evb10Touch has for
// I2C5; the two are separate copies because the touch driver's state is file-local and it is in use.
//
// Boot-args: evb10-audio=1        start (the one-shot above)
//            evb10-audio-addr=N   the codec's 7 bit address (default 0x11)
//            evb10-audio-khz=N    the SCL rate, 100..400 (default 100)
#include <IOKit/IOLib.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdint.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

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

// ---------------------------------------------------------------------------------------------------------------- bring-up

// The clock of I2C7 (clk-rk3588.c of the vendor kernel, the pattern of I2C5 in package/Evb10Touch): CLKSEL_CON(38) bit 12 chooses 200 / 100
// MHz, CLKGATE_CON(10) bit 14 gates its pclk and CLKGATE_CON(11) bit 6 the functional clock. The firmware may leave them running; only a
// gate found closed is opened. The log shows what was found, because the bit numbers are the part that was not read off this board.
uint32_t clockRate() {
  const uint32_t sel = rd(gCru, 0x398), g10 = rd(gCru, 0x828), g11 = rd(gCru, 0x82c);
  printf("evb10: audio: CRU sel38 %08x gate10 %08x gate11 %08x (i2c7: %s MHz, pclk %s, clk %s)\n", sel, g10, g11,
         (sel >> 12) & 1 ? "100" : "200", (g10 >> 14) & 1 ? "GATED" : "on", (g11 >> 6) & 1 ? "GATED" : "on");
  if ((g10 >> 14) & 1) wr(gCru, 0x828, 1u << (14 + 16));
  if ((g11 >> 6) & 1) wr(gCru, 0x82c, 1u << (6 + 16));
  return (sel >> 12) & 1 ? 100000000u : 200000000u;
}

// GPIO1_D0 / D1 are the two low nibbles of BUS_IOC's GPIO1D_IOMUX_SEL_L (0x38): function 9 is I2C7 (the touch driver's GPIO1B_H at 0x2c has
// the same layout). The write has the upper half as the write mask.
void pins() {
  const uint32_t mux = rd(gIoc, 0x38);
  printf("evb10: audio: IOC GPIO1D_L %08x (D0/D1 want 9/9)\n", mux);
  if ((mux & 0xff) != 0x99) {
    wr(gIoc, 0x38, 0x00ff0000u | 0x0099u);
    printf("evb10: audio: IOC GPIO1D_L now %08x\n", rd(gIoc, 0x38));
  }
}

void dumpRegisters() {
  uint8_t value[0x35] = {};
  unsigned failed = 0;
  for (unsigned r = 0; r < sizeof value; ++r) {
    if (codecRead(static_cast<uint8_t>(r), &value[r])) ++failed;
  }
  for (unsigned r = 0; r < sizeof value; r += 8) {
    char line[96];
    unsigned at = 0;
    for (unsigned i = r; i < r + 8 && i < sizeof value; ++i) at += static_cast<unsigned>(snprintf(line + at, sizeof line - at, " %02x", value[i]));
    printf("evb10: audio: regs %02x:%s\n", r, line);
  }
  printf("evb10: audio: register dump: %u of %u reads failed\n", failed, static_cast<unsigned>(sizeof value));
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
  printf("evb10: audio: write check reg %02x: was %02x, wrote %02x (%d), read %02x (%d), restored %02x (%d, read %d): %s\n", kReg, before,
         test, w, after, r, before, w2, restored, (!w && !r && !w2 && !r2 && (after & 0x3f) == test && restored == before) ? "PASS" : "FAIL");
}

thread_call_t gCall;

void run(thread_call_param_t, thread_call_param_t) {
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
  pins();
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
}

}  // namespace

// Called by the display probe once the HID services are up (package/LegacyDisplayProbe.cpp).
extern "C" void evb10_audio_start(void) {
  unsigned on = 0;
  if (!PE_parse_boot_argn("evb10-audio", &on, sizeof on) || on != 1 || gCall) return;
  gCall = thread_call_allocate(run, nullptr);
  if (!gCall) return;
  printf("evb10: audio: starting\n");
  thread_call_enter(gCall);
}
