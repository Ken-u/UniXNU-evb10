// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Evb10Wifi: the Wi-Fi module of the Rockchip RK3588 EVB10, an Ampak AP6398S (Broadcom BCM4359) on the SDIO host at 0xfe2d0000.
//
// This is the first step of the Wi-Fi port (docs/network.md): power the module, bring the card to the "selected" state over SDIO and
// read what it is, the way tools/board/sdio-probe.py of uefi-evb10 does from U-Boot. Nothing is downloaded and no interface is
// created. The host is the polled DW-MSHC (Synopsys, v2.70a) in 1-bit mode at 400 kHz; every command is a register write and a poll
// of RINTSTS, so the code needs neither the interrupt (SPI 204) nor DMA.
//
// Board facts (stock device tree, checked on the board by the U-Boot probe):
//   * sdiom0 pins, function 2: GPIO2_A6/A7 = D0/D1, GPIO2_B0/B1 = D2/D3, GPIO2_B2 = CMD, GPIO2_B3 = CLK. Their IOMUX registers are
//     GPIO2A_IOMUX_SEL_H and GPIO2B_IOMUX_SEL_L of BUS_IOC (0xfd5f8044 / 0xfd5f8048). The offsets of the drive-strength and pull
//     tables in pinctrl-rockchip.c look the same but are other registers: written as function selects they leave CLK low.
//   * WL_REG_ON = GPIO0_C4 (high = on). The module's 32.768 kHz low-power clock is the CLKOUT of the HYM8563 RTC, enabled after reset.
//   * cclk_src_sdio is CLKSEL_CON(172) (0xfd7c05b0) bits 9:8 (0 = GPLL, 1 = CPLL, 2 = 24 MHz) and 7:2 (divider - 1); its gate, the
//     host's hclk gate and the two resets are bits 3, 2 (and 0 for the bus clock) of CLKGATE_CON(75) (0xfd7c092c) and bits 2 and 3
//     of SOFTRST_CON(75) (0xfd7c0b2c); the card clock reaches the host unchanged and the host's CLKDIV divides it.
//
// Boot-args: evb10-wifi=1   power up and identify (once, from a thread call after the display probe)
#include <IOKit/IOLib.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdint.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

extern "C" {
// Steps reached: 0 not started, 1 module powered, 2 card answers CMD5, 3 card selected, 4 CIS read, 5 chip id read. Then the chip id
// (CHIPID register of ChipCommon), the CIS manufacturer / product id, and the number of IO functions the card reports.
volatile uint32_t evb10_wifi_stats[5];
}

namespace {

constexpr uint64_t kSdio = 0xfe2d0000ULL;
constexpr uint64_t kGpio0 = 0xfd8a0000ULL;
constexpr uint64_t kGpio2 = 0xfec30000ULL;
constexpr uint64_t kBusIoc = 0xfd5f8000ULL;
constexpr uint64_t kCru = 0xfd7c0000ULL;

// DW-MSHC registers
enum : uint32_t {
  kCtrl = 0x00, kPwren = 0x04, kClkDiv = 0x08, kClkSrc = 0x0c, kClkEna = 0x10, kTmout = 0x14, kCtype = 0x18, kBlkSiz = 0x1c, kBytCnt = 0x20,
  kIntMask = 0x24, kCmdArg = 0x28, kCmd = 0x2c, kResp0 = 0x30, kRintSts = 0x44, kStatus = 0x48, kFifoTh = 0x4c, kVerId = 0x6c,
};
enum : uint32_t {
  kCtrlReset = 1u << 0, kCtrlFifoReset = 1u << 1, kCtrlDmaReset = 1u << 2,
  kCmdStart = 1u << 31, kCmdUpdateClock = 1u << 21, kCmdSendInit = 1u << 15, kCmdWaitPrevData = 1u << 13, kCmdCheckCrc = 1u << 8,
  kCmdRespLong = 1u << 7, kCmdRespExpect = 1u << 6,
  kIntResponseError = 1u << 1, kIntCommandDone = 1u << 2, kIntRespCrc = 1u << 6, kIntRespTimeout = 1u << 8, kIntHardwareLock = 1u << 12,
};

volatile uint32_t *gSdio, *gGpio0, *gGpio2, *gIoc, *gCru;

inline uint32_t rd(volatile uint32_t *base, uint32_t off) { return base[off / 4]; }
inline void wr(volatile uint32_t *base, uint32_t off, uint32_t value) { base[off / 4] = value; }

uint64_t nowUs() {
  uint64_t ns;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000;
}

void mapAll() {
  gSdio = reinterpret_cast<volatile uint32_t *>(ml_io_map(kSdio, 0x1000));
  gGpio0 = reinterpret_cast<volatile uint32_t *>(ml_io_map(kGpio0, 0x1000));
  gGpio2 = reinterpret_cast<volatile uint32_t *>(ml_io_map(kGpio2, 0x1000));
  gIoc = reinterpret_cast<volatile uint32_t *>(ml_io_map(kBusIoc, 0x1000));
  gCru = reinterpret_cast<volatile uint32_t *>(ml_io_map(kCru, 0x1000));
}

// ---------------------------------------------------------------------------------------------------------------- pins, power, clock

// The mask for a CRU or IOC write sits in the upper 16 bits.
inline uint32_t masked(uint32_t mask, uint32_t value) { return (mask << 16) | (value & mask); }

void setPins() {
  wr(gIoc, 0x44, 0xff002200);  // GPIO2_A6 / A7: function 2 (sdiom0_d0, sdiom0_d1)
  wr(gIoc, 0x48, 0xffff2222);  // GPIO2_B0..B3: function 2 (d2, d3, cmd, clk)
}

// GPIO0_C4 is pin 20 of bank 0: bit 4 of the high half-word registers SWPORT_DR_H (0x04) and SWPORT_DDR_H (0x0c).
void wlRegOn(bool on) {
  wr(gGpio0, 0x04, masked(1u << 4, on ? 1u << 4 : 0));
  wr(gGpio0, 0x0c, masked(1u << 4, 1u << 4));
}

// The firmware leaves the SDIO clocks running and the resets released; they are written anyway because the cost is nothing and a
// gate found closed would make the first host register read hang the CPU.
void openClocks() {
  const uint32_t gate = rd(gCru, 0x92c), reset = rd(gCru, 0xb2c), sel = rd(gCru, 0x5b0);
  printf("evb10: wifi: CRU gate75 %08x softrst75 %08x sel172 %08x before\n", gate, reset, sel);
  wr(gCru, 0x92c, masked(0xd, 0));        // hclk_sdio_root (bit 0), HCLK_SDIO (bit 2), cclk_src_sdio (bit 3): ungated
  wr(gCru, 0xb2c, masked(0xc, 0));        // SRST_H_SDIO (bit 2), SRST_SDIO (bit 3): released
  wr(gCru, 0x5b0, masked(0x3fc, 2u << 8));  // cclk_src_sdio = 24 MHz, divider 1
}

// ---------------------------------------------------------------------------------------------------------------- host

int waitClear(uint32_t off, uint32_t bits, uint64_t timeoutUs) {
  const uint64_t began = nowUs();
  while (rd(gSdio, off) & bits) {
    if (nowUs() - began > timeoutUs) return -1;
    IODelay(1);
  }
  return 0;
}

// Tell the card clock domain about a change of CLKDIV / CLKENA.
int updateClock() {
  wr(gSdio, kCmd, kCmdStart | kCmdUpdateClock | kCmdWaitPrevData);
  return waitClear(kCmd, kCmdStart, 100000);
}

int hostInit(uint32_t clkDiv) {
  wr(gSdio, kPwren, 1);
  wr(gSdio, kCtrl, kCtrlReset | kCtrlFifoReset | kCtrlDmaReset);
  if (waitClear(kCtrl, kCtrlReset | kCtrlFifoReset | kCtrlDmaReset, 100000)) return -1;
  wr(gSdio, kIntMask, 0);
  wr(gSdio, kRintSts, 0xffffffff);
  wr(gSdio, kTmout, 0xffffff40);
  wr(gSdio, kCtype, 0);  // 1-bit
  wr(gSdio, kClkEna, 0);
  wr(gSdio, kClkSrc, 0);
  wr(gSdio, kClkDiv, clkDiv);
  if (updateClock()) return -2;
  wr(gSdio, kClkEna, 1);
  if (updateClock()) return -3;
  return 0;
}

// One command without data. Returns 0 and the response, -1 on a response timeout, -2 on a CRC or response error, -3 if the host
// does not answer.
int command(unsigned index, uint32_t arg, uint32_t flags, uint32_t *response) {
  if (waitClear(kCmd, kCmdStart, 100000)) return -3;
  wr(gSdio, kRintSts, 0xffffffff);
  wr(gSdio, kCmdArg, arg);
  wr(gSdio, kCmd, kCmdStart | kCmdWaitPrevData | flags | index);
  const uint64_t began = nowUs();
  uint32_t rint;
  for (;;) {
    rint = rd(gSdio, kRintSts);
    if (rint & (kIntCommandDone | kIntHardwareLock)) break;
    if (nowUs() - began > 500000) return -3;
    IODelay(2);
  }
  if (rint & kIntHardwareLock) return -3;
  if (rint & kIntRespTimeout) return -1;
  if (response) *response = rd(gSdio, kResp0);
  if (rint & (kIntRespCrc | kIntResponseError)) return -2;
  return 0;
}

// CMD52 (IO_RW_DIRECT) of function `func`. Returns the data byte, or a negative error (-1 timeout, -2 error, -3 host, -4 card flags).
int io(unsigned func, uint32_t addr, bool write, uint8_t value) {
  uint32_t r = 0;
  const uint32_t arg = (func << 28) | (addr << 9) | (write ? 0x80000000u : 0) | value;
  const int c = command(52, arg, kCmdRespExpect | kCmdCheckCrc, &r);
  if (c) return c;
  if (r & 0xcb00) return -4;  // COM_CRC_ERROR, ILLEGAL_COMMAND, ERROR, FUNCTION_NUMBER, OUT_OF_RANGE
  return static_cast<int>(r & 0xff);
}

// ---------------------------------------------------------------------------------------------------------------- bring-up

struct Identity {
  uint32_t ocr = 0, rca = 0, functions = 0, cisPointer = 0, manufacturer = 0, product = 0, chipId = 0;
};

bool identify(Identity &id) {
  uint32_t r = 0;
  // Card reset (the 74 clocks of the SD initialisation ride on CMD0's send-init bit), then the SDIO voltage handshake.
  int c = command(0, 0, kCmdSendInit, nullptr);
  if (c) {
    printf("evb10: wifi: CMD0 failed %d\n", c);
    return false;
  }
  c = command(5, 0, kCmdRespExpect, &r);
  if (c) {
    printf("evb10: wifi: CMD5 failed %d (no card answer: pins, WL_REG_ON, the 32 kHz clock or the host clock)\n", c);
    return false;
  }
  id.ocr = r & 0xffffff;
  id.functions = (r >> 28) & 7;
  printf("evb10: wifi: CMD5 OCR %06x, %u IO functions\n", id.ocr, id.functions);
  evb10_wifi_stats[0] = 2;
  evb10_wifi_stats[4] = id.functions;
  const uint64_t began = nowUs();
  do {
    c = command(5, id.ocr & 0xff8000, kCmdRespExpect, &r);
    if (!c && (r & 0x80000000u)) break;
    IODelay(1000);
  } while (nowUs() - began < 1000000);
  if (c || !(r & 0x80000000u)) {
    printf("evb10: wifi: card not ready (CMD5 %d, response %08x)\n", c, r);
    return false;
  }
  c = command(3, 0, kCmdRespExpect | kCmdCheckCrc, &r);
  if (c) {
    printf("evb10: wifi: CMD3 failed %d\n", c);
    return false;
  }
  id.rca = r >> 16;
  c = command(7, id.rca << 16, kCmdRespExpect | kCmdCheckCrc, &r);
  if (c) {
    printf("evb10: wifi: CMD7 failed %d\n", c);
    return false;
  }
  printf("evb10: wifi: RCA %u selected\n", id.rca);
  evb10_wifi_stats[0] = 3;

  const int cccr = io(0, 0x00, false, 0), caps = io(0, 0x08, false, 0);
  int p[3];
  for (unsigned i = 0; i < 3; ++i) p[i] = io(0, 0x09 + i, false, 0);
  if (cccr < 0 || caps < 0 || p[0] < 0 || p[1] < 0 || p[2] < 0) {
    printf("evb10: wifi: CCCR read failed (%d %d %d %d %d)\n", cccr, caps, p[0], p[1], p[2]);
    return false;
  }
  id.cisPointer = static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16;
  printf("evb10: wifi: CCCR revision %02x, capabilities %02x, CIS at %x\n", cccr, caps, id.cisPointer);

  // The CIS is a list of tuples (code, link, data). CISTPL_MANFID (0x20) holds the manufacturer and the product ID.
  uint8_t cis[16];
  for (unsigned i = 0; i < sizeof cis; ++i) {
    const int b = io(0, id.cisPointer + i, false, 0);
    if (b < 0) {
      printf("evb10: wifi: CIS read failed at %u (%d)\n", i, b);
      return false;
    }
    cis[i] = static_cast<uint8_t>(b);
  }
  for (unsigned i = 0; i + 6 <= sizeof cis; ++i) {
    if (cis[i] == 0x20 && cis[i + 1] == 4) {
      id.manufacturer = cis[i + 2] | cis[i + 3] << 8;
      id.product = cis[i + 4] | cis[i + 5] << 8;
      break;
    }
  }
  printf("evb10: wifi: CIS %02x %02x %02x %02x %02x %02x %02x %02x: MANFID %04x:%04x\n", cis[0], cis[1], cis[2], cis[3], cis[4], cis[5], cis[6],
         cis[7], id.manufacturer, id.product);
  evb10_wifi_stats[0] = 4;
  evb10_wifi_stats[2] = id.manufacturer << 16 | id.product;

  // Function 1 is the backplane. It needs the ALP clock before the window registers answer; ChipCommon's CHIPID is its first register.
  if (io(0, 0x02, true, 0x02) < 0) return false;                 // CCCR IOEx: enable function 1
  const uint64_t ioBegan = nowUs();
  int ready;
  while ((ready = io(0, 0x03, false, 0)) >= 0 && !(ready & 2) && nowUs() - ioBegan < 100000) IODelay(100);
  if (ready < 0 || !(ready & 2)) {
    printf("evb10: wifi: function 1 not ready (IORx %d)\n", ready);
    return false;
  }
  io(1, 0x1000e, true, 0x08);                                     // CHIPCLKCSR: ALP request
  const uint64_t alpBegan = nowUs();
  int csr;
  while ((csr = io(1, 0x1000e, false, 0)) >= 0 && !(csr & 0x40) && nowUs() - alpBegan < 100000) IODelay(100);
  if (csr < 0 || !(csr & 0x40)) {
    printf("evb10: wifi: no ALP clock (CHIPCLKCSR %d)\n", csr);
    return false;
  }
  io(1, 0x1000a, true, 0x00);                                     // backplane window 0x18000000 (ChipCommon)
  io(1, 0x1000b, true, 0x00);
  io(1, 0x1000c, true, 0x18);
  uint32_t chip = 0;
  for (unsigned i = 0; i < 4; ++i) {
    const int b = io(1, i, false, 0);
    if (b < 0) {
      printf("evb10: wifi: CHIPID read failed (%d)\n", b);
      return false;
    }
    chip |= static_cast<uint32_t>(b) << (8 * i);
  }
  id.chipId = chip;
  evb10_wifi_stats[0] = 5;
  evb10_wifi_stats[1] = chip;
  printf("evb10: wifi: CHIPCLKCSR %02x, ChipCommon CHIPID %08x: chip %x revision %u\n", csr, chip, chip & 0xffff, (chip >> 16) & 0xf);
  return true;
}

thread_call_t gCall;

void start(thread_call_param_t, thread_call_param_t) {
  mapAll();
  openClocks();
  printf("evb10: wifi: SDIO host VERID %08x\n", rd(gSdio, kVerId));
  setPins();
  wlRegOn(false);
  IOSleep(100);
  wlRegOn(true);
  IOSleep(300);  // the pwrseq of the device tree waits 200 ms after power-on
  evb10_wifi_stats[0] = 1;
  printf("evb10: wifi: WL_REG_ON high\n");
  if (hostInit(0x1e)) {  // 24 MHz / (2 * 30) = 400 kHz
    printf("evb10: wifi: host init failed\n");
    return;
  }
  unsigned high = 0;
  for (unsigned i = 0; i < 20; ++i) {  // GPIO2_B3 = CLK pad: the 2.5 us period of 400 kHz shows as both levels when sampled 1 us apart
    high += (rd(gGpio2, 0x70) >> 11) & 1;
    IODelay(1);
  }
  printf("evb10: wifi: CLK pad high in %u of 20 samples%s\n", high, high == 0 || high == 20 ? " (NOT toggling)" : "");
  Identity id;
  const bool ok = identify(id);
  if (ok && id.manufacturer == 0x02d0 && id.product == 0x4359 && (id.chipId & 0xffff) == 0x4359)
    printf("evb10: wifi: AP6398S (BCM4359 revision %u) identified over SDIO\n", (id.chipId >> 16) & 0xf);
  else
    printf("evb10: wifi: identification %s (steps reached: %u)\n", ok ? "found something else" : "failed", evb10_wifi_stats[0]);
}

}  // namespace

// Called by the display probe once the HID services are up (package/LegacyDisplayProbe.cpp).
extern "C" void evb10_wifi_start(void) {
  unsigned on = 0;
  if (!PE_parse_boot_argn("evb10-wifi", &on, sizeof on) || on != 1 || gCall) return;
  gCall = thread_call_allocate(start, nullptr);
  if (!gCall) {
    printf("evb10: wifi: no thread call\n");
    return;
  }
  thread_call_enter(gCall);
}
