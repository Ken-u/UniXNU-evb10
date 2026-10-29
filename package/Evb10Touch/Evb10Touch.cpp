// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Evb10Touch: the touch panel of the Rockchip RK3588 EVB10's DSI display, read by polling.
//
// The panel's touch controller sits on I2C5 (0xfead0000, pins GPIO1_B6 = SCL / GPIO1_B7 = SDA, function 9, the "i2c5m3" routing of the
// board's device tree); the device tree names two candidates on that bus, a Goodix GT1x at 0x14 and a Hynitron CST3240 at 0x5a, with
// reset on GPIO3_C1 and the interrupt on GPIO3_C0. Which one is fitted is not known from the tree, so the first thing this does is look:
// it resets the controller, scans the bus and reads the Goodix product id or the Hynitron report block.
//
// What it produces is the finger set of NativeHidInput (the HID digitizer source of UniXNU's sheng device, drivers/NativeHidInput of this
// repository): a seqlock-protected list of contacts in panel pixels (native_touch_seq / _count / _xs / _ys / _ids) and a sink that is
// called after every published set. Nothing here needs the HID side: with only the log (evb10: touch: ...) it shows what the controller
// reports, which is how the axes were checked.
//
// The I2C master is the polled one of U-Boot's rk_i2c.c (GPL-2.0+, Rockchip) rewritten for the kernel; the Goodix register map
// (0x8140 product id, 0x8048 configuration, 0x814e status, 0x814f contacts) is the GT9xx/GT1x one.
//
// Boot-args: evb10-touch=1        start (reset, scan, poll every 10 ms, publish)
//            evb10-touch-swap=1   exchange the axes (after the flips are applied it is the last step)
//            evb10-touch-flipx=1  mirror X
//            evb10-touch-flipy=1  mirror Y
//            evb10-touch-xmax=N / -ymax=N   the sensor grid (default: what a Goodix reports as configured; this panel needs 720 x 1280)
//            evb10-touch-addr=N   use this 7 bit address (default: the first that answers of 0x14, 0x5d, 0x5a)
//            evb10-touch-log=N    N lines of finger positions at most per second (default 10)
//            evb10-touch-demo=S   scripted gestures without a finger, from S seconds after the first poll: a swipe up, then every 7 s a swipe left / right
#include <IOKit/IOLib.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdint.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

extern "C" {
// The finger set for NativeHidInput: odd while being written. Same names and layout as UniXNU-sheng's touch probe.
volatile uint32_t native_touch_seq;
volatile uint32_t native_touch_count;
volatile uint32_t native_touch_hid_reports;
volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
// Frames read, frames with a contact, bus errors, last raw X / Y, and the controller's kind (0 none, 1 Goodix, 2 Hynitron).
volatile uint32_t evb10_touch_stats[6];
}

namespace {

constexpr uint64_t kI2c5 = 0xfead0000ULL;
constexpr uint64_t kGpio3 = 0xfec40000ULL;
constexpr uint64_t kBusIoc = 0xfd5f8000ULL;
constexpr uint64_t kCru = 0xfd7c0000ULL;
constexpr unsigned kPanelW = 1080, kPanelH = 1920;
constexpr unsigned kFingers = 5;

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

volatile uint32_t *gI2c, *gGpio3, *gIoc, *gCru;

inline uint32_t rd(volatile uint32_t *base, uint32_t off) { return base[off / 4]; }
inline void wr(volatile uint32_t *base, uint32_t off, uint32_t value) { base[off / 4] = value; }

uint64_t nowUs() {
  uint64_t ns;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000;
}

uint32_t divUp(uint64_t a, uint64_t b) { return static_cast<uint32_t>((a + b - 1) / b); }

// ---------------------------------------------------------------------------------------------------------------- GPIO

// GPIO3_C0 (INT) and GPIO3_C1 (RST) are pins 16 and 17 of bank 3: the high half-word registers, bit 0 and 1, written with a mask in
// the upper 16 bits.
enum { kPinInt = 0, kPinRst = 1 };
void gpioOut(unsigned pin, bool high) {
  wr(gGpio3, 0x04, (1u << (pin + 16)) | (high ? 1u << pin : 0));  // SWPORT_DR_H
  wr(gGpio3, 0x0c, (1u << (pin + 16)) | (1u << pin));             // SWPORT_DDR_H: output
}
void gpioIn(unsigned pin) { wr(gGpio3, 0x0c, (1u << (pin + 16))); }
bool gpioGet(unsigned pin) { return (rd(gGpio3, 0x70) >> (pin + 16)) & 1; }

// ---------------------------------------------------------------------------------------------------------------- I2C

struct I2c {
  unsigned cfg = 0;
  bool speedSet = false;
  static constexpr uint64_t kTimeoutUs = 100000;

  void disable() {
    wr(gI2c, kIen, 0);
    wr(gI2c, kIpd, kIntAll);
    wr(gI2c, kCon, 0);
  }

  // rk_i2c_adapter_clk of U-Boot (controllers with the version field set, which this one has).
  bool setSpeed(uint32_t sclRate, uint32_t clkRate) {
    unsigned startSetup = 0, speed;
    uint32_t minLowNs, minHighNs, maxRiseNs, maxFallNs;
    if (sclRate <= 100000 && sclRate >= 1000) {
      startSetup = 1; speed = 100; minLowNs = 4700; minHighNs = 4000; maxRiseNs = 1000; maxFallNs = 300;
    } else if (sclRate <= 400000 && sclRate >= 100000) {
      speed = 400; minLowNs = 1300; minHighNs = 600; maxRiseNs = 300; maxFallNs = 300;
    } else {
      return false;
    }
    (void)speed;
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
    speedSet = true;
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

// ---------------------------------------------------------------------------------------------------------------- controllers

enum Kind { kNone = 0, kGoodix = 1, kHynitron = 2 };
Kind gKind = kNone;
uint8_t gAddr = 0;
unsigned gXMax = kPanelW, gYMax = kPanelH;

struct Contact {
  int id, x, y;
};

bool gtRead(uint16_t reg, uint8_t *buf, unsigned len) {
  const uint8_t w[2] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg)};
  return gBus.xfer(gAddr, w, 2, buf, len) == 0;
}
bool gtWriteByte(uint16_t reg, uint8_t value) {
  const uint8_t w[3] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg), value};
  return gBus.xfer(gAddr, w, 3, nullptr, 0) == 0;
}

// One poll of a Goodix: the number of contacts, or -1 when there is no new report (and -2 on a bus error).
uint32_t gStatusSeen;  // polls that read a non-zero 0x814e, and the last one: nothing at all would mean the chip never reports
uint8_t gLastStatus;
int gtPoll(Contact *out) {
  uint8_t status = 0;
  if (!gtRead(0x814e, &status, 1)) return -2;
  if (status) {
    gLastStatus = status;
    gStatusSeen = gStatusSeen + 1;
  }
  if (!(status & 0x80)) return -1;
  unsigned n = status & 0x0f;
  if (n > 4) n = 4;  // 8 bytes each, one 32 byte FIFO
  uint8_t raw[32] = {};
  bool ok = true;
  if (n) ok = gtRead(0x814f, raw, n * 8);
  gtWriteByte(0x814e, 0);
  if (!ok) return -2;
  for (unsigned i = 0; i < n; ++i) {
    const uint8_t *p = raw + i * 8;
    out[i].id = p[0];
    out[i].x = p[1] | (p[2] << 8);
    out[i].y = p[3] | (p[4] << 8);
  }
  return static_cast<int>(n);
}

bool cstRead(uint16_t reg, uint8_t *buf, unsigned len) {
  const uint8_t w[2] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg)};
  return gBus.xfer(gAddr, w, 2, buf, len) == 0;
}
void cstAck() {
  const uint8_t w[3] = {0xd0, 0x00, 0xab};
  gBus.xfer(gAddr, w, 3, nullptr, 0);
}
// Hynitron CST3xx: 0xd000 holds the first contact (5 bytes), the count in byte 5 and 0xab in byte 6; more contacts follow at 0xd007.
int cstPoll(Contact *out) {
  uint8_t buf[7 + 4 * 5 + 1] = {};
  if (!cstRead(0xd000, buf, 7)) return -2;
  if (buf[6] != 0xab) {
    cstAck();
    return -1;
  }
  unsigned n = buf[5] & 0x7f;
  if (n > 5) {
    cstAck();
    return -1;
  }
  if (n > 1 && !cstRead(0xd007, buf + 7, (n - 1) * 5 + 1)) return -2;
  cstAck();
  unsigned kept = 0;
  for (unsigned i = 0; i < n && kept < kFingers; ++i) {
    const unsigned at = i == 0 ? 0 : i * 5 + 2;
    if ((buf[at] & 0x0f) != 0x06) continue;  // 6: pressed
    out[kept].id = buf[at] >> 4;
    out[kept].x = (buf[at + 1] << 4) | ((buf[at + 3] >> 4) & 0x0f);
    out[kept].y = (buf[at + 2] << 4) | (buf[at + 3] & 0x0f);
    ++kept;
  }
  return static_cast<int>(kept);
}

// ---------------------------------------------------------------------------------------------------------------- publication

void (*gSink)(void *);
void *gSinkContext;

bool gSwap, gFlipX, gFlipY;

void publish(unsigned count, const Contact *c) {
  int ids[kFingers], xs[kFingers], ys[kFingers];
  for (unsigned i = 0; i < count; ++i) {
    long x = c[i].x, y = c[i].y;
    // Raw controller units to panel pixels.
    const long mx = gXMax > 1 ? gXMax - 1 : 1, my = gYMax > 1 ? gYMax - 1 : 1;
    if (x > mx) x = mx;
    if (y > my) y = my;
    x = x * (kPanelW - 1) / mx;
    y = y * (kPanelH - 1) / my;
    if (gFlipX) x = (kPanelW - 1) - x;
    if (gFlipY) y = (kPanelH - 1) - y;
    if (gSwap) {  // the axes of a swapped panel have the other panel dimension
      const long t = x * (kPanelH - 1) / (kPanelW - 1);
      x = y * (kPanelW - 1) / (kPanelH - 1);
      y = t;
    }
    ids[i] = c[i].id;
    xs[i] = static_cast<int>(x);
    ys[i] = static_cast<int>(y);
  }
  native_touch_seq = native_touch_seq + 1;
  __asm__ volatile("dmb ish" ::: "memory");
  native_touch_count = count;
  for (unsigned i = 0; i < count; ++i) {
    native_touch_xs[i] = xs[i];
    native_touch_ys[i] = ys[i];
    native_touch_ids[i] = ids[i];
  }
  __asm__ volatile("dmb ish" ::: "memory");
  native_touch_seq = native_touch_seq + 1;
  void (*sink)(void *) = __atomic_load_n(&gSink, __ATOMIC_ACQUIRE);
  if (sink) sink(gSinkContext);
}

// ---------------------------------------------------------------------------------------------------------------- bring-up

thread_call_t gCall;
bool gInitDone;
unsigned gLogPerSecond = 10;
uint32_t gBeat;

void arm(unsigned ms) {
  uint64_t deadline;
  clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
  thread_call_enter_delayed(gCall, deadline);
}

void mapAll() {
  gI2c = reinterpret_cast<volatile uint32_t *>(ml_io_map(kI2c5, 0x1000));
  gGpio3 = reinterpret_cast<volatile uint32_t *>(ml_io_map(kGpio3, 0x1000));
  gIoc = reinterpret_cast<volatile uint32_t *>(ml_io_map(kBusIoc, 0x1000));
  gCru = reinterpret_cast<volatile uint32_t *>(ml_io_map(kCru, 0x1000));
}

// The clock of I2C5: CLKSEL_CON(38) bit 10 chooses 200 / 100 MHz, CLKGATE_CON(10) bit 12 gates its pclk and CLKGATE_CON(11) bit 4 the
// functional clock (clk-rk3588.c of the vendor kernel). The firmware leaves them running; only a gate found closed is opened.
uint32_t clockRate() {
  const uint32_t sel = rd(gCru, 0x398), g10 = rd(gCru, 0x828), g11 = rd(gCru, 0x82c);
  printf("evb10: touch: CRU sel38 %08x gate10 %08x gate11 %08x (i2c5: %s MHz, pclk %s, clk %s)\n", sel, g10, g11,
         (sel >> 10) & 1 ? "100" : "200", (g10 >> 12) & 1 ? "GATED" : "on", (g11 >> 4) & 1 ? "GATED" : "on");
  if ((g10 >> 12) & 1) wr(gCru, 0x828, 1u << (12 + 16));
  if ((g11 >> 4) & 1) wr(gCru, 0x82c, 1u << (4 + 16));
  return (sel >> 10) & 1 ? 100000000u : 200000000u;
}

void pins() {
  const uint32_t mux = rd(gIoc, 0x2c), ctl = rd(gIoc, 0x70);
  printf("evb10: touch: IOC GPIO1B_H %08x (B6/B7 want 9/9), GPIO3C_L %08x (C0/C1 want 0/0)\n", mux, ctl);
  if (((mux >> 8) & 0xff) != 0x99) wr(gIoc, 0x2c, 0xff000000u | 0x9900u);
  if ((ctl & 0xff) != 0) wr(gIoc, 0x70, 0x00ff0000u);
}

// RST low with INT high (address 0x14 for a Goodix), RST high, then INT is an input again.
void reset() {
  gpioOut(kPinInt, true);
  gpioOut(kPinRst, false);
  IOSleep(12);
  gpioOut(kPinRst, true);
  IOSleep(8);
  gpioIn(kPinInt);
  IOSleep(80);
  printf("evb10: touch: reset done, INT %d RST %d\n", gpioGet(kPinInt), gpioGet(kPinRst));
}

void init() {
  mapAll();
  if (!gI2c || !gGpio3 || !gIoc || !gCru) {
    printf("evb10: touch: cannot map the controllers\n");
    return;
  }
  const uint32_t version = (rd(gI2c, kCon) >> 16) & 0x1ff;
  const uint32_t rate = clockRate();
  pins();
  gBus.disable();
  printf("evb10: touch: I2C5 version %u, clock %u Hz, ", version, rate);
  if (!gBus.setSpeed(100000, rate)) {
    printf("cannot set 100 kHz\n");
    return;
  }
  printf("CLKDIV %08x\n", rd(gI2c, kClkDiv));
  reset();

  unsigned found = 0;
  char line[160];
  unsigned at = 0;
  for (unsigned a = 0x08; a < 0x78; ++a) {
    if (gBus.probe(static_cast<uint8_t>(a))) {
      if (at + 6 < sizeof line) at += static_cast<unsigned>(snprintf(line + at, sizeof line - at, " 0x%02x", a));
      ++found;
    }
  }
  printf("evb10: touch: I2C5 scan: %u device(s) answer:%s\n", found, found ? line : " none");

  unsigned forced = 0;
  PE_parse_boot_argn("evb10-touch-addr", &forced, sizeof forced);
  const uint8_t candidates[] = {0x14, 0x5d, 0x5a};
  for (uint8_t a : candidates) {
    if (forced && forced != a) continue;
    if (!forced && !gBus.probe(a)) continue;
    gAddr = a;
    if (a == 0x5a) {
      uint8_t raw[7] = {};
      const bool ok = cstRead(0xd000, raw, 7);
      printf("evb10: touch: 0x5a: read d000 %s: %02x %02x %02x %02x %02x %02x %02x\n", ok ? "ok" : "FAILED", raw[0], raw[1], raw[2], raw[3],
             raw[4], raw[5], raw[6]);
      if (ok) {
        gKind = kHynitron;
        break;
      }
    } else {
      uint8_t id[4] = {};
      if (gtRead(0x8140, id, 4)) {
        uint8_t cfg[5] = {};
        gtRead(0x8048, cfg, 5);
        const unsigned xm = cfg[0] | (cfg[1] << 8), ym = cfg[2] | (cfg[3] << 8);
        printf("evb10: touch: Goodix at 0x%02x, product id %c%c%c%c (%02x %02x %02x %02x), configured %ux%u, %u contacts\n", a,
               id[0] >= 32 && id[0] < 127 ? id[0] : '.', id[1] >= 32 && id[1] < 127 ? id[1] : '.', id[2] >= 32 && id[2] < 127 ? id[2] : '.',
               id[3] >= 32 && id[3] < 127 ? id[3] : '.', id[0], id[1], id[2], id[3], xm, ym, cfg[4] & 0xf);
        if (xm >= 100 && xm <= 8192 && ym >= 100 && ym <= 8192) {
          gXMax = xm;
          gYMax = ym;
        }
        gKind = kGoodix;
        break;
      }
    }
  }
  evb10_touch_stats[5] = gKind;
  if (gKind == kNone) {
    printf("evb10: touch: no touch controller found (tried 0x14 0x5d 0x5a)\n");
    return;
  }
  unsigned flag = 0;
  gSwap = PE_parse_boot_argn("evb10-touch-swap", &flag, sizeof flag) && flag;
  flag = 0;
  gFlipX = PE_parse_boot_argn("evb10-touch-flipx", &flag, sizeof flag) && flag;
  flag = 0;
  gFlipY = PE_parse_boot_argn("evb10-touch-flipy", &flag, sizeof flag) && flag;
  unsigned logs = 0;
  if (PE_parse_boot_argn("evb10-touch-log", &logs, sizeof logs)) gLogPerSecond = logs;
  // The size of the sensor grid, when what the controller reports is not it: touch3-5 measured X 9..696 and Y 10..1270 on a Goodix GT1158
  // whose stored configuration says 800x480 (it is 720x1280, the 1080x1920 panel at 2/3).
  unsigned maximum = 0;
  if (PE_parse_boot_argn("evb10-touch-xmax", &maximum, sizeof maximum) && maximum >= 16 && maximum <= 16384) gXMax = maximum;
  maximum = 0;
  if (PE_parse_boot_argn("evb10-touch-ymax", &maximum, sizeof maximum) && maximum >= 16 && maximum <= 16384) gYMax = maximum;
  printf("evb10: touch: %s at 0x%02x, axes %ux%u -> %ux%u, swap %d flipx %d flipy %d\n", gKind == kGoodix ? "Goodix" : "Hynitron", gAddr, gXMax,
         gYMax, kPanelW, kPanelH, gSwap, gFlipX, gFlipY);
  gInitDone = true;
}

// evb10-touch-demo=SECS: scripted gestures, no finger needed, for measuring the frame rate of real animations. From SECS seconds after the first poll, every
// 7 s: a swipe up from the bottom edge (unlock, close an app), then alternately a swipe to the left and to the right across the middle (pages of the home
// screen, the camera and widgets screens of the lock screen), each 0.8 s long, one contact, in the controller's raw units like a real finger.
// Returns 1 while a contact is down (c is where), 0 once when it lifts, -1 when it is not the demo's turn.
int demoPoll(Contact *c) {
  static unsigned start = ~0u;
  static uint64_t began;
  static bool down;
  if (start == ~0u) {
    start = 0;
    PE_parse_boot_argn("evb10-touch-demo", &start, sizeof start);
    began = nowUs();
  }
  if (!start) return -1;
  const int64_t t = static_cast<int64_t>((nowUs() - began) / 1000) - static_cast<int64_t>(start) * 1000;   // ms since the first gesture
  const int64_t phase = t >= 0 ? t % 7000 : -1;
  if (phase < 0 || phase >= 800) {
    if (down) {
      down = false;
      return 0;
    }
    return -1;
  }
  const unsigned index = static_cast<unsigned>(t / 7000);
  const int xs = static_cast<int>(gXMax), ys = static_cast<int>(gYMax);
  const int p = static_cast<int>(phase);   // 0..799
  int x, y;
  if (index == 0) {                          // up, from the bottom edge to a third of the screen
    x = xs / 2;
    y = ys * 97 / 100 - (ys * 62 / 100) * p / 800;
  } else if (index % 2 == 1) {               // left
    y = ys / 2;
    x = xs * 85 / 100 - (xs * 70 / 100) * p / 800;
  } else {                                   // right
    y = ys / 2;
    x = xs * 15 / 100 + (xs * 70 / 100) * p / 800;
  }
  if (!down) {
    down = true;
    printf("evb10: touch: demo gesture %u: %s\n", index, index == 0 ? "swipe up" : (index % 2 ? "swipe left" : "swipe right"));
  }
  c->id = 0;
  c->x = x;
  c->y = y;
  return 1;
}

void tick(thread_call_param_t, thread_call_param_t) {
  if (!gInitDone) {
    init();
    if (!gInitDone) return;  // nothing to poll
  }
  static Contact contacts[kFingers], previous[kFingers];
  static unsigned previousCount;
  static uint64_t logWindow;
  static unsigned logged;
  static uint32_t errors;
  int n = gKind == kGoodix ? gtPoll(contacts) : cstPoll(contacts);
  {
    Contact scripted;
    const int d = demoPoll(&scripted);
    if (d >= 0) {
      n = d;
      if (d) contacts[0] = scripted;
    }
  }
  if (n == -2) {
    ++errors;
    evb10_touch_stats[2] = errors;
    if (errors < 5 || errors % 1000 == 0) printf("evb10: touch: bus error #%u\n", errors);
  } else if (n >= 0) {
    evb10_touch_stats[0] = evb10_touch_stats[0] + 1;
    if (n) evb10_touch_stats[1] = evb10_touch_stats[1] + 1;
    evb10_touch_stats[3] = static_cast<uint32_t>(contacts[0].x);
    evb10_touch_stats[4] = static_cast<uint32_t>(contacts[0].y);
    bool same = static_cast<unsigned>(n) == previousCount;
    for (int i = 0; same && i < n; ++i) same = contacts[i].id == previous[i].id && contacts[i].x == previous[i].x && contacts[i].y == previous[i].y;
    if (!same) {
      const uint64_t now = nowUs();
      if (now - logWindow >= 1000000) {
        logWindow = now;
        logged = 0;
      }
      const bool edge = static_cast<unsigned>(n) != previousCount;  // a finger down or up is always logged
      if (edge || logged < gLogPerSecond) {
        ++logged;
        char text[120];
        unsigned at = 0;
        for (int i = 0; i < n && at + 24 < sizeof text; ++i)
          at += static_cast<unsigned>(snprintf(text + at, sizeof text - at, " #%d(%d,%d)", contacts[i].id, contacts[i].x, contacts[i].y));
        printf("evb10: touch: %d contact(s)%s\n", n, at ? text : "");
      }
      publish(static_cast<unsigned>(n), contacts);
      for (int i = 0; i < n; ++i) previous[i] = contacts[i];
      previousCount = static_cast<unsigned>(n);
    }
  }
  if (++gBeat % 1000 == 0)
    printf("evb10: touch: t+%us frames %u with contact %u errors %u last raw (%u,%u) status reads %u last 0x%02x INT %d, HID reports %u\n", gBeat / 100,
           evb10_touch_stats[0], evb10_touch_stats[1], evb10_touch_stats[2], evb10_touch_stats[3], evb10_touch_stats[4], gStatusSeen,
           gLastStatus, gpioGet(kPinInt), native_touch_hid_reports);
  arm(10);
}

}  // namespace

extern "C" void native_touch_set_sink(void (*sink)(void *), void *context) {
  gSinkContext = context;
  __atomic_store_n(&gSink, sink, __ATOMIC_RELEASE);
}

// Called by the display probe once the HID services are up (package/LegacyDisplayProbe.cpp).
extern "C" void evb10_touch_start(void) {
  unsigned on = 0;
  if (!PE_parse_boot_argn("evb10-touch", &on, sizeof on) || on != 1 || gCall) return;
  gCall = thread_call_allocate(tick, nullptr);
  if (!gCall) {
    printf("evb10: touch: no thread call\n");
    return;
  }
  printf("evb10: touch: starting\n");
  thread_call_enter(gCall);
}
