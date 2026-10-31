// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Evb10Wifi: the Wi-Fi module of the Rockchip RK3588 EVB10, an Ampak AP6398S (Broadcom BCM4359) on the SDIO host at 0xfe2d0000.
//
// The Wi-Fi port (docs/network.md) in one file: power the module, bring the card to the "selected" state over SDIO, download the
// firmware, talk to it (SDPCM/BCDC), scan, join a WPA2-PSK network with the firmware's supplicant and move Ethernet frames, which
// WifiNet.cpp turns into the interface en0 for the OS. The host is the polled DW-MSHC (Synopsys, v2.70a): every command is a register
// write and a poll of RINTSTS, data goes through the FIFO, so the code needs neither the interrupt (SPI 204) nor DMA. Identification
// runs in 1-bit mode at 400 kHz as the U-Boot probe (tools/board/sdio-probe.py of uefi-evb10) does it.
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
// With evb10-wifi=2 it goes on: the card to 4 bits and about 25 MHz, CMD53 (PIO through the host's FIFO, byte mode for registers and
// 64-byte blocks for the images), the chip's ARM CR4 halted, the firmware and the nvram written into its RAM and read back, the core
// released, and the check that the firmware runs: HT clock available and function 2 ready. The images are the vendor's, embedded by
// scripts/build-wifi-firmware.py from .cache/firmware (wifi_fw.inc); without them level 2 stops after the bus switch. The chip side
// follows Linux's brcmfmac (chip.c, sdio.c); the core addresses are the EROM of this chip, read with the same CMD53 on the board.
//
// Boot-args: evb10-wifi=1   power up and identify (once, from a thread call after the display probe)
//            evb10-wifi=2   identify, then switch the bus, download the firmware and start it
//            evb10-wifi=3   then talk to the firmware (SDPCM/BCDC over function 2): version, MAC address, three scans listed on the console
//            evb10-wifi=4   as 3, then join the network of .cache/firmware/join.txt (WPA2-PSK by the firmware supplicant) and run a self-test over the
//                           air: DHCP, ARP, ping of the router and of the DNS server
//            evb10-wifi=5   as 4 without the self-test: the station interface below serves the board's network code
//            evb10-wifi-join=0   with 4 or 5: do not join the file's network at boot (the join then comes from the OS, from Settings)
//            evb10-wifi-join=2   as 0, and the scans list only the file's network (a test aid: a scripted touch finds it in a fixed row of Settings)
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdint.h>
#include <string.h>

#include "nw_abi.h"
#include "wifi_fw.inc"  // kWifiFirmware / kWifiNvram and their sizes (scripts/build-wifi-firmware.py)

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
// WifiNet.cpp (scripts/build-evb10-wifinet.py): the network side, whose classes are registered late (initializer made external).
extern "C" void evb10_wifi_net_static_init(void) __asm__("__GLOBAL__sub_I_WifiNet.cpp");
extern "C" void evb10_wifi_net_start(const uint8_t mac[6]);
extern kmod_info_t g_kernel_kmod_info;
// (kern/thread.h is not included: it changes how IOLib.h's typed allocators compile under -Werror)
extern "C" kern_return_t kernel_thread_start(thread_continue_t continuation, void *parameter, thread_t *new_thread);
extern "C" void thread_deallocate(thread_t thread);
extern "C" kern_return_t thread_terminate(thread_t thread);
extern "C" thread_t current_thread(void);

extern "C" {
// Steps reached: 0 not started, 1 module powered, 2 card answers CMD5, 3 card selected, 4 CIS read, 5 chip id read, 6 bus at 4 bits and
// 25 MHz, 7 image in RAM and verified, 8 firmware running (function 2 ready). Then the chip id
// (CHIPID register of ChipCommon), the CIS manufacturer / product id, the number of IO functions the card reports, and the RAM size.
volatile uint32_t evb10_wifi_stats[6];
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
constexpr uint32_t kFifoReg = 0x200;  // the data FIFO answers on every word of 0x200..0xfff
constexpr uint32_t kCmdDataExpected = 1u << 9, kCmdWrite = 1u << 10;
constexpr uint32_t kIntDataDone = 1u << 3, kIntDataCrc = 1u << 7, kIntDataTimeout = 1u << 9,
                   kIntStartBit = 1u << 13, kIntEndBit = 1u << 15;
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

// ---------------------------------------------------------------------------------------------------------------- bus switch, CMD53

constexpr unsigned kBlock = 64;    // function 1 block size
constexpr unsigned kChunk = 512;   // bytes per CMD53 (the host FIFO holds 1 KiB)
constexpr uint32_t kSdioCoreBase = 0x18005000, kCr4Base = 0x18002000, kCr4Wrap = 0x18102000, kRamBase = 0x160000;
constexpr uint32_t kD11Wrap[2] = {0x18101000, 0x18104000};  // the two 802.11 cores' wrappers
constexpr uint32_t kIoctl = 0x408, kResetCtl = 0x800;
constexpr uint32_t kCr4Halt = 0x20, kD11PhyClockEn = 0x4, kD11PhyReset = 0x8;

void fifoReset() {
  wr(gSdio, kCtrl, kCtrlFifoReset);
  waitClear(kCtrl, kCtrlFifoReset, 100000);
}

constexpr unsigned kFifoWords = 256;  // depth of the host's FIFO (HCON)
constexpr uint32_t kIntDataErrors = kIntDataCrc | kIntDataTimeout | kIntStartBit | kIntEndBit;

// CMD53 on the incrementing address `addr` of function `func`. `bytes` is a multiple of 4. In block mode (`blk` != 0: `bytes` is a
// multiple of `blk`) the argument counts blocks, in byte mode (`blk` == 0, `bytes` below 512) bytes. The words move through the host's
// FIFO by the CPU (polled): the first 256 words of a write are put in before the command, the rest as the card drains it; a read takes
// what the card has put in. The host stops the card clock when the FIFO is full or empty, so a transfer longer than the FIFO is safe
// as long as the CPU keeps up.
// 0 on success; -1/-2/-3 as command(), -4 card flags, -5 data timeout (software), -6 data error.
int cmd53(bool write, unsigned func, uint32_t addr, unsigned blk, uint32_t *words, unsigned bytes) {
  const unsigned total = bytes / 4;
  const unsigned count = blk ? bytes / blk : bytes;
  const uint32_t arg = (write ? 0x80000000u : 0) | func << 28 | (blk ? 1u << 27 : 0) | 1u << 26 | addr << 9 | (count & 0x1ff);
  fifoReset();
  wr(gSdio, kBlkSiz, blk ? blk : bytes);
  wr(gSdio, kBytCnt, bytes);
  unsigned done = 0;
  if (write)
    for (; done < total && done < kFifoWords; ++done) wr(gSdio, kFifoReg, words[done]);
  uint32_t r = 0;
  int c = command(53, arg, kCmdRespExpect | kCmdCheckCrc | kCmdDataExpected | (write ? kCmdWrite : 0), &r);
  if (c) return c;
  if (r & 0xcb00) return -4;
  const uint64_t began = nowUs();
  uint32_t rint;
  for (;;) {
    rint = rd(gSdio, kRintSts);
    const unsigned fill = (rd(gSdio, kStatus) >> 17) & 0x1fff;
    if (write) {
      for (unsigned room = kFifoWords - fill; room && done < total; --room, ++done) wr(gSdio, kFifoReg, words[done]);
    } else {
      for (unsigned n = fill; n && done < total; --n, ++done) words[done] = rd(gSdio, kFifoReg);
    }
    if (rint & kIntDataErrors) return -6;
    if ((rint & kIntDataDone) && done == total) break;
    if (rint & kIntDataDone && !write && !((rd(gSdio, kStatus) >> 17) & 0x1fff)) return -6;  // ended with words missing
    if (nowUs() - began > 500000) return -5;
  }
  return 0;
}

// The F1 (backplane) form of it: 4 bytes in byte mode, or blocks of kBlock.
int transfer(bool write, unsigned func, uint32_t addr, uint32_t *words, unsigned bytes) {
  return cmd53(write, func, addr, bytes >= kBlock ? kBlock : 0, words, bytes);
}

// The card to 4-bit mode (CCCR bus interface control), the host's CTYPE to match, then the card clock to about 25 MHz: cclk_src_sdio from
// GPLL (1188 MHz) divided by 24, which the CRU's clock generator halves again, with the host's own divider bypassed.
bool busSwitch() {
  if (io(0, 0x07, true, 0x82) < 0) return false;  // 4-bit bus, card detect pull-up off
  wr(gSdio, kCtype, 1);
  wr(gSdio, kClkEna, 0);
  if (updateClock()) return false;
  wr(gCru, 0x5b0, masked(0x3fc, 23u << 2));       // mux 0 (GPLL), divider 23 + 1
  wr(gSdio, kClkDiv, 0);
  if (updateClock()) return false;
  wr(gSdio, kClkEna, 1);
  if (updateClock()) return false;
  // Function 1 moves data in 64-byte blocks (FBR1 block size, registers 0x110/0x111 of function 0).
  if (io(0, 0x110, true, kBlock & 0xff) < 0 || io(0, 0x111, true, kBlock >> 8) < 0) return false;
  return io(0, 0x00, false, 0) >= 0;
}

uint32_t gWindow = 0xffffffff;

// The backplane is seen through a 32 KiB window of function 1 whose base is in registers 0x1000a..0x1000c.
bool setWindow(uint32_t addr) {
  const uint32_t bar = addr & ~0x7fffu;
  if (bar == gWindow) return true;
  for (unsigned i = 0; i < 3; ++i)
    if (io(1, 0x1000a + i, true, (bar >> (8 + 8 * i)) & 0xff) < 0) return false;
  gWindow = bar;
  return true;
}

constexpr uint32_t kBp32 = 0x8000;  // SBSDIO_SB_ACCESS_2_4B_FLAG: 32-bit accesses

bool bpRead32(uint32_t addr, uint32_t *value) {
  return setWindow(addr) && transfer(false, 1, (addr & 0x7fff) | kBp32, value, 4) == 0;
}

bool bpWrite32(uint32_t addr, uint32_t value) {
  return setWindow(addr) && transfer(true, 1, (addr & 0x7fff) | kBp32, &value, 4) == 0;
}

// Copy to / from the chip's memory in the pieces CMD53 takes: not across a window edge, blocks of 64 where it is at least 64 bytes.
int ramAccess(bool write, uint32_t addr, uint8_t *data, unsigned len) {
  uint32_t words[kChunk / 4];
  while (len) {
    unsigned n = len > kChunk ? kChunk : len;
    const unsigned windowLeft = 0x8000 - (addr & 0x7fff);
    if (n > windowLeft) n = windowLeft;
    unsigned moved = n >= kBlock ? n & ~(kBlock - 1) : (n + 3) & ~3u;   // bytes on the bus
    const unsigned used = moved < n ? moved : n;                        // bytes of `data` they cover
    if (!setWindow(addr)) return -1;
    if (write) {
      memset(words, 0, sizeof words);
      memcpy(words, data, used);
    }
    const int c = transfer(write, 1, (addr & 0x7fff) | kBp32, words, moved);
    if (c) return c;
    if (!write) memcpy(data, words, used);
    addr += used;
    data += used;
    len -= used;
  }
  return 0;
}

bool ramVerify(uint32_t addr, const uint8_t *data, unsigned len) {
  uint8_t back[kChunk];
  for (unsigned at = 0; at < len;) {
    unsigned n = len - at > kChunk ? kChunk : len - at;
    if (ramAccess(false, addr + at, back, n)) {
      printf("evb10: wifi: read back failed at %x\n", addr + at);
      return false;
    }
    if (memcmp(back, data + at, n)) {
      unsigned k = 0;
      while (back[k] == data[at + k]) ++k;
      printf("evb10: wifi: image differs at %x: wrote %02x read %02x\n", addr + at + k, data[at + k], back[k]);
      return false;
    }
    at += n;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------- the chip's cores

// The AXI wrapper of a core: IOCTL (clock and core bits) and RESET_CTL, as brcmf_chip_ai_coredisable / _resetcore.
void coreDisable(uint32_t wrap, uint32_t prereset, uint32_t reset) {
  uint32_t v = 0;
  bpRead32(wrap + kResetCtl, &v);
  if (!(v & 1)) {
    bpWrite32(wrap + kIoctl, prereset | 2 | 1);  // FGC | CLK
    bpRead32(wrap + kIoctl, &v);
    bpWrite32(wrap + kResetCtl, 1);
    IODelay(20);
    for (unsigned i = 0; i < 300; ++i) {
      bpRead32(wrap + kResetCtl, &v);
      if (v == 1) break;
      IODelay(1);
    }
  }
  bpWrite32(wrap + kIoctl, reset | 2 | 1);
  bpRead32(wrap + kIoctl, &v);
}

void coreReset(uint32_t wrap, uint32_t prereset, uint32_t reset, uint32_t postreset) {
  uint32_t v = 0;
  coreDisable(wrap, prereset, reset);
  for (unsigned count = 0;; ++count) {
    bpRead32(wrap + kResetCtl, &v);
    if (!(v & 1) || count > 50) break;
    bpWrite32(wrap + kResetCtl, 0);
    IODelay(50);
  }
  bpWrite32(wrap + kIoctl, postreset | 1);
  bpRead32(wrap + kIoctl, &v);
}

// brcmf_chip_set_passive for a CR4 chip: the ARM halted (core clock on), the 802.11 cores disabled.
void armPassive() {
  uint32_t v = 0;
  bpRead32(kCr4Wrap + kIoctl, &v);
  coreReset(kCr4Wrap, v & kCr4Halt, kCr4Halt, kCr4Halt);
  for (uint32_t wrap : kD11Wrap) coreDisable(wrap, kD11PhyReset | kD11PhyClockEn, kD11PhyClockEn);
}

// brcmf_chip_tcm_ramsize: the CR4 core's TCM banks (capability register: bank counts; bank index / info registers).
uint32_t ramSize() {
  uint32_t cap = 0, size = 0;
  if (!bpRead32(kCr4Base + 0x04, &cap)) return 0;
  const unsigned banks = (cap & 0xf) + ((cap & 0xf0) >> 4);
  for (unsigned i = 0; i < banks; ++i) {
    uint32_t info = 0;
    if (!bpWrite32(kCr4Base + 0x40, i) || !bpRead32(kCr4Base + 0x44, &info)) return 0;
    unsigned unit = 8192;
    if (info & 0x200) unit >>= 3;
    size += ((info & 0x7f) + 1) * unit;
  }
  return size;
}

bool waitClockCsr(uint8_t want, unsigned timeoutMs, int *last) {
  const uint64_t began = nowUs();
  int csr;
  while ((csr = io(1, 0x1000e, false, 0)) >= 0 && (csr & want) != want && nowUs() - began < timeoutMs * 1000ull) IODelay(200);
  *last = csr;
  return csr >= 0 && (csr & want) == want;
}

uint32_t gRamSize;

bool downloadAndRun() {
  if (!kWifiFirmwareSize || !kWifiNvramSize) {
    printf("evb10: wifi: no firmware embedded (scripts/build-wifi-firmware.py found nothing in .cache/firmware)\n");
    return false;
  }
  armPassive();
  uint32_t ioctl = 0, reset = 0;
  bpRead32(kCr4Wrap + kIoctl, &ioctl);
  bpRead32(kCr4Wrap + kResetCtl, &reset);
  const uint32_t ram = ramSize();
  gRamSize = ram;
  evb10_wifi_stats[5] = ram;
  printf("evb10: wifi: ARM CR4 halted (IOCTL %x RESET_CTL %x), RAM %x bytes at %x\n", ioctl, reset, ram, kRamBase);
  if (!ram || kWifiFirmwareSize + kWifiNvramSize > ram) {
    printf("evb10: wifi: RAM size unusable\n");
    return false;
  }
  const uint64_t began = nowUs();
  const int w = ramAccess(true, kRamBase, const_cast<uint8_t *>(kWifiFirmware), kWifiFirmwareSize);
  if (w) {
    printf("evb10: wifi: firmware write failed %d\n", w);
    return false;
  }
  const uint32_t nvramAt = kRamBase + ram - kWifiNvramSize;
  const int n = ramAccess(true, nvramAt, const_cast<uint8_t *>(kWifiNvram), kWifiNvramSize);
  if (n) {
    printf("evb10: wifi: nvram write failed %d\n", n);
    return false;
  }
  const uint64_t written = nowUs();
  if (!ramVerify(kRamBase, kWifiFirmware, kWifiFirmwareSize) || !ramVerify(nvramAt, kWifiNvram, kWifiNvramSize)) return false;
  printf("evb10: wifi: firmware %u bytes at %x and nvram %u bytes at %x written in %llu ms and verified in %llu ms\n", kWifiFirmwareSize, kRamBase,
         kWifiNvramSize, nvramAt, static_cast<unsigned long long>((written - began) / 1000), static_cast<unsigned long long>((nowUs() - written) / 1000));
  evb10_wifi_stats[0] = 7;

  // brcmf_chip_set_active: clear the SDIO core's interrupt status, the reset vector (the image's first word) to address 0, release the ARM.
  const uint32_t rstvec = kWifiFirmware[0] | kWifiFirmware[1] << 8 | kWifiFirmware[2] << 16 | static_cast<uint32_t>(kWifiFirmware[3]) << 24;
  bpWrite32(kSdioCoreBase + 0x20, 0xffffffff);
  uint32_t vec = rstvec;
  if (ramAccess(true, 0, reinterpret_cast<uint8_t *>(&vec), 4)) {
    printf("evb10: wifi: reset vector write failed\n");
    return false;
  }
  coreReset(kCr4Wrap, kCr4Halt, 0, 0);
  bpRead32(kCr4Wrap + kIoctl, &ioctl);
  bpRead32(kCr4Wrap + kResetCtl, &reset);
  printf("evb10: wifi: ARM CR4 released (reset vector %08x, IOCTL %x RESET_CTL %x)\n", rstvec, ioctl, reset);
  const uint64_t released = nowUs();

  // The firmware has started when it has brought up the HT clock (the PLL) and enabled function 2.
  io(1, 0x1000e, true, 0x10);  // CHIPCLKCSR: HT request
  int csr = 0;
  const bool ht = waitClockCsr(0xc0, 1000, &csr);
  printf("evb10: wifi: HT clock %s after %llu ms (CHIPCLKCSR %02x)\n", ht ? "available" : "NOT available", static_cast<unsigned long long>((nowUs() - released) / 1000), csr);
  if (!ht) return false;
  bpWrite32(kSdioCoreBase + 0x48, 4u << 16);  // tosbmailboxdata: host protocol version 4
  io(0, 0x02, true, 0x06);                    // IOEx: functions 1 and 2
  int ready = 0;
  const uint64_t f2Began = nowUs();
  while ((ready = io(0, 0x03, false, 0)) >= 0 && !(ready & 4) && nowUs() - f2Began < 1000000) IODelay(500);
  printf("evb10: wifi: function 2 %s after %llu ms from the release (IORx %02x)\n", ready >= 0 && (ready & 4) ? "ready" : "NOT ready",
         static_cast<unsigned long long>((nowUs() - released) / 1000), ready);
  if (ready < 0 || !(ready & 4)) return false;
  evb10_wifi_stats[0] = 8;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------- function 2: SDPCM

// What brcmf_sdio_firmware_callback does after the firmware is up: the host interrupt mask, the F2 watermark (the chip raises a frame
// indication only when this many words are ready), function 2's block size, and the HT clock kept on (SBSDIO_FORCE_HT) so that nothing
// has to wake the backplane.
constexpr uint32_t kIntStatusReg = kSdioCoreBase + 0x20, kHostIntMaskReg = kSdioCoreBase + 0x24, kToSbMailbox = kSdioCoreBase + 0x40,
                   kToHostMailboxData = kSdioCoreBase + 0x4c;
constexpr uint32_t kIntFrame = 1u << 6, kIntHostMail = 1u << 7, kIntChipActive = 1u << 29;
constexpr uint32_t kHostIntMask = 0xf0 | kIntChipActive;
constexpr unsigned kF2Block = 512, kSdpcmHdr = 12, kFirstRead = 64;
constexpr uint32_t kMailAck = 2;
constexpr uint32_t kHmbDevReady = 2, kHmbFwReady = 8, kHmbFwHalt = 0x10;
constexpr unsigned kMaxFrame = 2048 + 256;

bool f2Setup() {
  bpWrite32(kHostIntMaskReg, kHostIntMask);
  io(1, 0x10008, true, 0x40);                              // SBSDIO_WATERMARK
  const int devctl = io(1, 0x10009, false, 0);             // SBSDIO_DEVICE_CTL
  if (devctl >= 0) io(1, 0x10009, true, static_cast<uint8_t>(devctl | 0x10));   // F2WM_ENAB
  io(1, 0x1001d, true, 0x40 | 0x80);                       // SBSDIO_FUNC1_MESBUSYCTRL: watermark and enable
  const int csr = io(1, 0x1000e, false, 0);
  if (csr >= 0) io(1, 0x1000e, true, static_cast<uint8_t>(csr | 0x02));         // SBSDIO_FORCE_HT
  if (io(0, 0x210, true, kF2Block & 0xff) < 0 || io(0, 0x211, true, kF2Block >> 8) < 0) return false;  // FBR2 block size
  return true;
}

alignas(8) uint8_t gRx[kMaxFrame];       // one frame read from function 2
alignas(8) uint8_t gTx[kMaxFrame];       // one frame to write
alignas(8) uint8_t gCtlIn[kMaxFrame];    // the last control (ioctl response) payload
uint32_t gCtlLen;
bool gCtlReady, gFwReady;
uint8_t gTxSeq = 255, gTxMax = 0, gRxSeq = 0;   // brcmfmac starts the transmit sequence at 255 and the credit window at 0
uint16_t gReqId;
uint32_t gSdpcmStats[8];          // frames in, frames out, control in, events, data in, header errors, read errors, credit waits

// A frame goes over function 2 as whole blocks of kF2Block where there are any, the rest in byte mode: what the MMC core does.
int f2Transfer(bool write, uint8_t *buf, unsigned bytes) {
  while (bytes) {
    unsigned n;
    unsigned blk = 0;
    if (bytes >= kF2Block) {
      n = bytes / kF2Block * kF2Block;
      if (n > 4 * kF2Block) n = 4 * kF2Block;
      blk = kF2Block;
    } else {
      n = bytes;
    }
    const int c = cmd53(write, 2, 0, blk, reinterpret_cast<uint32_t *>(buf), n);
    if (c) return c;
    buf += n;
    bytes -= n;
  }
  return 0;
}

inline uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
inline uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
inline void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
inline void put32(uint8_t *p, uint32_t v) { p[0] = v & 0xff; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
inline uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
inline uint32_t be32(const uint8_t *p) { return static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// The firmware's own console (brcmf_sdio_readconsole): sdpcm_shared at the last word of its RAM names the rte_console, whose log is a ring.
void dumpConsole() {
  uint32_t shared = 0;
  if (!gRamSize || !bpRead32(kRamBase + gRamSize - 4, &shared) || shared < kRamBase || shared >= kRamBase + gRamSize - 64) {
    printf("evb10: wifi: console: no shared structure (%08x)\n", shared);
    return;
  }
  uint8_t sh[48];
  if (ramAccess(false, shared, sh, sizeof sh)) return;
  const uint32_t con = le32(sh + 20);
  printf("evb10: wifi: console: shared %08x flags %08x trap %08x assert %08x console %08x\n", shared, le32(sh), le32(sh + 4), le32(sh + 8), con);
  if (con < kRamBase || con >= kRamBase + gRamSize - 32) return;
  uint8_t c[32];
  if (ramAccess(false, con, c, sizeof c)) return;
  const uint32_t buf = le32(c + 8), size = le32(c + 12), idx = le32(c + 16);
  if (buf < kRamBase || !size || size > 8192 || buf + size > kRamBase + gRamSize) {
    printf("evb10: wifi: console: ring %08x size %u idx %u\n", buf, size, idx);
    return;
  }
  static uint8_t ring[8192 + 8];
  if (ramAccess(false, buf, ring, size)) return;
  // oldest first: from idx to the end, then from the start up to idx
  static char text[8192 + 8];
  unsigned n = 0;
  for (unsigned i = 0; i < size; ++i) {
    const uint8_t ch = ring[(idx + i) % size];
    if (ch) text[n++] = static_cast<char>(ch);
  }
  text[n] = 0;
  for (unsigned at = 0; at < n;) {
    unsigned e = at;
    while (e < n && text[e] != '\n' && e - at < 110) ++e;
    printf("evb10: wifi: fw: %.*s\n", static_cast<int>(e - at), text + at);
    at = e < n && text[e] == '\n' ? e + 1 : e;
  }
}

bool creditOk(unsigned reserve = 0) {
  const uint8_t window = static_cast<uint8_t>(gTxMax - gTxSeq - reserve);
  return window != 0 && !(window & 0x80);
}

// The frame header: hardware (length, its complement), software (sequence, channel, data offset; flow control and credit come back).
int sdpcmSend(unsigned channel, const uint8_t *payload, unsigned length) {
  const unsigned total = kSdpcmHdr + length;
  if (total > kMaxFrame - 4) return -10;
  const unsigned padded = (total + 3) & ~3u;
  memset(gTx, 0, padded);
  put16(gTx, static_cast<uint16_t>(total));
  put16(gTx + 2, static_cast<uint16_t>(~total));
  put32(gTx + 4, gTxSeq | channel << 8 | kSdpcmHdr << 24);
  memcpy(gTx + kSdpcmHdr, payload, length);
  const int c = f2Transfer(true, gTx, padded);
  if (c) {
    gSdpcmStats[6]++;
    io(1, 0x1000d, true, 0x02);   // SFC_WF_TERM: abandon the frame in the chip
    return c;
  }
  gTxSeq = static_cast<uint8_t>(gTxSeq + 1);
  gSdpcmStats[1]++;
  return 0;
}

// ---- events and the BSS list (filled by the event handler below)

struct Station;
void handleEvent(const uint8_t *eth, unsigned length);
void handleData(const uint8_t *eth, unsigned length);

// One frame from function 2 (the header first, then the rest). Returns 1 for a frame, 0 for none, -1 for an error.
int rxOne() {
  int c = f2Transfer(false, gRx, kFirstRead);
  if (c) {
    gSdpcmStats[6]++;
    io(1, 0x1000d, true, 0x01);   // SFC_RF_TERM
    return -1;
  }
  const unsigned len = le16(gRx), chk = le16(gRx + 2);
  if (!(len | chk)) return 0;
  if (static_cast<uint16_t>(~(len ^ chk))) {
    gSdpcmStats[5]++;
    printf("evb10: wifi: bad frame header %02x%02x%02x%02x\n", gRx[0], gRx[1], gRx[2], gRx[3]);
    io(1, 0x1000d, true, 0x01);
    return -1;
  }
  const uint32_t sw = le32(gRx + 4), sw2 = le32(gRx + 8);
  const unsigned channel = (sw >> 8) & 0xf, offset = sw >> 24;
  if (len < kSdpcmHdr || len > kMaxFrame - 8 || offset < kSdpcmHdr || offset > len) {
    gSdpcmStats[5]++;
    printf("evb10: wifi: bad frame length %u offset %u channel %u\n", len, offset, channel);
    io(1, 0x1000d, true, 0x01);
    return -1;
  }
  const uint8_t window = (sw2 >> 8) & 0xff;
  gTxMax = static_cast<uint8_t>(window - gTxSeq) > 0x40 ? static_cast<uint8_t>(gTxSeq + 2) : window;
  if (len > kFirstRead) {
    const unsigned rest = (len - kFirstRead + 3) & ~3u;
    c = f2Transfer(false, gRx + kFirstRead, rest);
    if (c) {
      gSdpcmStats[6]++;
      printf("evb10: wifi: reading %u more bytes of a frame failed (%d)\n", rest, c);
      io(1, 0x1000d, true, 0x01);
      return -1;
    }
  }
  gRxSeq = static_cast<uint8_t>(gRxSeq + 1);
  gSdpcmStats[0]++;
  const uint8_t *body = gRx + offset;
  const unsigned bodyLen = len - offset;
  if (channel == 0) {
    gSdpcmStats[2]++;
    if (bodyLen <= sizeof gCtlIn) {
      memcpy(gCtlIn, body, bodyLen);
      gCtlLen = bodyLen;
      gCtlReady = true;
    }
  } else if (channel == 1 || channel == 2) {
    // BCDC header: flags (version in the top nibble), priority, flags2 (interface), data offset in words.
    if (bodyLen > 4 && (body[0] >> 4) == 2) {
      const unsigned skip = 4 + body[3] * 4u;
      if (bodyLen > skip) {
        const uint8_t *eth = body + skip;
        const unsigned ethLen = bodyLen - skip;
        if (ethLen >= 14 && eth[12] == 0x88 && eth[13] == 0x6c) {
          gSdpcmStats[3]++;
          handleEvent(eth, ethLen);
        } else if (ethLen >= 14) {
          gSdpcmStats[4]++;
          handleData(eth, ethLen);
        }
      }
    }
  }
  return 1;
}

// The mailbox: the chip tells the host it is ready (and which protocol version it speaks) or that it halted.
void hostMail() {
  uint32_t d = 0;
  if (!bpRead32(kToHostMailboxData, &d)) return;
  bpWrite32(kToSbMailbox, kMailAck);
  if (d & kHmbFwHalt) printf("evb10: wifi: the firmware reports a halt (mailbox %08x)\n", d);
  if (d & (kHmbDevReady | kHmbFwReady)) {
    gFwReady = true;
    printf("evb10: wifi: firmware ready, protocol version %u (mailbox %08x)\n", (d >> 16) & 0xff, d);
  }
}

// One pass over the chip's status: new events, frames, mailbox. Returns true if there was something.
bool service() {
  uint32_t st = 0;
  if (!bpRead32(kIntStatusReg, &st)) return false;
  st &= kHostIntMask;
  if (!st) return false;
  bpWrite32(kIntStatusReg, st);
  if (st & kIntHostMail) hostMail();
  if (st & kIntFrame) {
    for (unsigned i = 0; i < 16; ++i)
      if (rxOne() <= 0) break;
  }
  return true;
}


// ---------------------------------------------------------------------------------------------------------------- BCDC: ioctls and iovars

constexpr uint32_t kCmdUp = 2, kCmdSetInfra = 20, kCmdSetAp = 118, kCmdSetScanChannelTime = 185, kCmdSetScanUnassocTime = 187, kCmdGetVar = 262, kCmdSetVar = 263;
constexpr int kErrTimeout = -1001, kErrSend = -1002, kErrSize = -1003;

alignas(8) uint8_t gCtlOut[kMaxFrame];
alignas(8) uint8_t gVar[kMaxFrame];

// Waits for the next pass over the chip's status: a short spin first, then sleeps of a millisecond.
void idle(unsigned &spins) {
  if (spins++ < 40) IODelay(100);
  else IOSleep(1);
}

// One ioctl, as brcmf_proto_bcdc_msg / _cmplt do: the command header (command, length, flags with the request id, status) and the data
// go out on channel 0, the answer comes back on channel 0 with the same id. `fwerr` gets the firmware's status when it refused.
// Returns 0, or kErrTimeout / kErrSend / kErrSize, or the firmware's (negative) error code.
int dcmd(uint32_t cmd, bool set, const uint8_t *in, unsigned inLen, uint8_t *out, unsigned outLen, unsigned timeoutMs = 3000) {
  if (16 + inLen > kMaxFrame - 64) return kErrSize;
  const uint16_t id = ++gReqId;
  put32(gCtlOut, cmd);
  put32(gCtlOut + 4, inLen);
  put32(gCtlOut + 8, static_cast<uint32_t>(id) << 16 | (set ? 2u : 0u));
  put32(gCtlOut + 12, 0);
  if (inLen) memcpy(gCtlOut + 16, in, inLen);
  uint64_t began = nowUs();
  unsigned spins = 0;
  while (!creditOk()) {
    gSdpcmStats[7]++;
    if (!service()) idle(spins);
    if (nowUs() - began > 2000000) return kErrTimeout;
  }
  gCtlReady = false;
  if (sdpcmSend(0, gCtlOut, 16 + inLen)) return kErrSend;
  began = nowUs();
  spins = 0;
  for (;;) {
    if (gCtlReady) {
      gCtlReady = false;
      if (gCtlLen >= 16) {
        const uint32_t flags = le32(gCtlIn + 8);
        if ((flags >> 16) == id) {
          if (out && outLen) {
            const unsigned have = gCtlLen - 16;
            memcpy(out, gCtlIn + 16, have < outLen ? have : outLen);
          }
          return (flags & 1) ? static_cast<int32_t>(le32(gCtlIn + 12)) : 0;
        }
      }
    }
    if (nowUs() - began > timeoutMs * 1000ull) return kErrTimeout;
    if (!service()) idle(spins);
  }
}

// An iovar is an ioctl on the command GET_VAR / SET_VAR whose data starts with the variable's name and a NUL (brcmf_create_iovar).
int iovarGet(const char *name, uint8_t *out, unsigned outLen) {
  const unsigned nl = strlen(name) + 1;
  if (nl + outLen > sizeof gVar) return kErrSize;
  memset(gVar, 0, nl + outLen);
  memcpy(gVar, name, nl);
  return dcmd(kCmdGetVar, false, gVar, nl + outLen, out, outLen);
}

int iovarSet(const char *name, const void *data, unsigned len) {
  const unsigned nl = strlen(name) + 1;
  if (nl + len > sizeof gVar) return kErrSize;
  memcpy(gVar, name, nl);
  if (len) memcpy(gVar + nl, data, len);
  return dcmd(kCmdSetVar, true, gVar, nl + len, nullptr, 0);
}

int iovarSetInt(const char *name, uint32_t value) {
  uint8_t v[4];
  put32(v, value);
  return iovarSet(name, v, 4);
}

// The same for one interface of the chip ("bsscfg:" prefix, the interface index ahead of the data; index 0 is the station).
int bsscfgSet(const char *name, const void *data, unsigned len) {
  char full[48];
  snprintf(full, sizeof full, "bsscfg:%s", name);
  uint8_t tmp[300];
  if (len + 4 > sizeof tmp) return kErrSize;
  put32(tmp, 0);
  if (len) memcpy(tmp + 4, data, len);
  return iovarSet(full, tmp, len + 4);
}

[[maybe_unused]] int bsscfgSetInt(const char *name, uint32_t value) {
  uint8_t v[4];
  put32(v, value);
  return bsscfgSet(name, v, 4);
}

int cmdSetInt(uint32_t cmd, uint32_t value) {
  uint8_t v[4];
  put32(v, value);
  return dcmd(cmd, true, v, 4, nullptr, 0);
}

// ---------------------------------------------------------------------------------------------------------------- the BSS list

// brcmf_bss_info_le: the record of one access point in a scan result.
struct BssInfoLe {
  uint32_t version, length;
  uint8_t bssid[6];
  uint16_t beaconPeriod, capability;
  uint8_t ssidLen, ssid[32];
  struct { uint32_t count; uint8_t rates[16]; } rateset;
  uint16_t chanspec, atimWindow;
  uint8_t dtimPeriod;
  int16_t rssi;
  int8_t phyNoise;
  uint8_t nCap;
  uint32_t nbssCap;
  uint8_t ctlCh;
  uint32_t reserved32;
  uint8_t flags, reserved[3], basicMcs[16];
  uint16_t ieOffset;
  uint32_t ieLength;
  int16_t snr;
};
static_assert(__builtin_offsetof(BssInfoLe, ssidLen) == 18 && __builtin_offsetof(BssInfoLe, chanspec) == 72 &&
              __builtin_offsetof(BssInfoLe, rssi) == 78 && __builtin_offsetof(BssInfoLe, ctlCh) == 88 &&
              __builtin_offsetof(BssInfoLe, ieOffset) == 116 && __builtin_offsetof(BssInfoLe, ieLength) == 120, "brcmf_bss_info_le layout");

constexpr unsigned kMaxBss = 48;
struct Bss {
  nw_bss_info info;
  uint64_t seenMs;
};
Bss gBss[kMaxBss];
unsigned gBssCount;
// evb10-wifi-join=2 (a test aid): only the join file's network is listed, so that a scripted touch finds it in a fixed row of the Settings list.
uint8_t gOnlySsid[32];
unsigned gOnlySsidLen;
IOLock *gLock;

uint64_t nowMs() { return nowUs() / 1000; }

void addBss(const uint8_t *rec, unsigned avail) {
  if (avail < sizeof(BssInfoLe)) return;
  BssInfoLe b;
  memcpy(&b, rec, sizeof b);
  if (b.length > avail || b.ssidLen > 32) return;
  if (gOnlySsidLen && (b.ssidLen != gOnlySsidLen || memcmp(b.ssid, gOnlySsid, b.ssidLen))) return;
  nw_bss_info n;
  memset(&n, 0, sizeof n);
  memcpy(n.bssid, b.bssid, 6);
  n.ssid_len = b.ssidLen;
  memcpy(n.ssid, b.ssid, b.ssidLen);
  const unsigned channel = b.ctlCh ? b.ctlCh : (b.chanspec & 0xff);
  const bool fiveGhz = (b.chanspec & 0xc000) == 0xc000;
  n.freq_mhz = fiveGhz ? 5000 + 5 * channel : (channel == 14 ? 2484 : 2407 + 5 * channel);
  n.signal_dbm = static_cast<int8_t>(b.rssi);
  n.caps = b.capability;
  n.beacon_int = b.beaconPeriod;
  if (b.ieOffset >= sizeof b && b.ieOffset <= b.length) {
    unsigned ie = b.length - b.ieOffset;
    if (ie > b.ieLength) ie = b.ieLength;
    if (ie > sizeof n.ie) ie = sizeof n.ie;
    memcpy(n.ie, rec + b.ieOffset, ie);
    n.ie_len = static_cast<uint16_t>(ie);
  }
  IOLockLock(gLock);
  unsigned at = gBssCount;
  for (unsigned i = 0; i < gBssCount; ++i)
    if (!memcmp(gBss[i].info.bssid, n.bssid, 6)) {
      at = i;
      break;
    }
  if (at == gBssCount) {
    if (gBssCount < kMaxBss) {
      ++gBssCount;
    } else {
      at = 0;
      for (unsigned i = 1; i < gBssCount; ++i)
        if (gBss[i].info.signal_dbm < gBss[at].info.signal_dbm) at = i;
    }
  }
  gBss[at].info = n;
  gBss[at].seenMs = nowMs();
  IOLockUnlock(gLock);
}

// ---------------------------------------------------------------------------------------------------------------- events

enum : uint32_t {
  kEvSetSsid = 0, kEvJoin = 1, kEvAuth = 3, kEvDeauth = 5, kEvDeauthInd = 6, kEvAssoc = 7, kEvDisassoc = 11, kEvDisassocInd = 12, kEvLink = 16,
  kEvRoam = 19, kEvPskSup = 46, kEvIf = 54, kEvEscanResult = 69,
};
constexpr uint32_t kEvStatusPartial = 8;

struct Event {
  uint32_t type, status, reason, flags, authType, dataLen;
  uint8_t addr[6];
  uint8_t ifidx;
  const uint8_t *data;
};

void onConnEvent(const Event &e);

volatile bool gScanActive, gScanFinished;
unsigned gScanResults;
bool gLinkUp;   // the last link event (read by the station interface)

void onEscanResult(const Event &e) {
  if (e.status == kEvStatusPartial) {
    // brcmf_escan_result_le: buflen, version, sync_id, bss_count, then bss_count BSS records.
    if (e.dataLen < 12 || le16(e.data + 8) != 0x1234) return;
    const unsigned count = le16(e.data + 10);
    const uint8_t *rec = e.data + 12;
    unsigned left = e.dataLen - 12;
    for (unsigned i = 0; i < count && left >= 8; ++i) {
      const unsigned length = le32(rec + 4);
      if (length > left || length < sizeof(BssInfoLe)) break;
      addBss(rec, length);
      gScanResults++;
      rec += length;
      left -= length;
    }
  } else {
    gScanActive = false;
    gScanFinished = true;
  }
}

void handleEvent(const uint8_t *eth, unsigned length) {
  // Ethernet header (14), Broadcom header (10: subtype, length, version, OUI, user subtype), event message (48), then the data.
  if (length < 14 + 10 + 48) return;
  const uint8_t *m = eth + 24;
  Event e;
  e.flags = be16(m + 2);
  e.type = be32(m + 4);
  e.status = be32(m + 8);
  e.reason = be32(m + 12);
  e.authType = be32(m + 16);
  e.dataLen = be32(m + 20);
  memcpy(e.addr, m + 24, 6);
  e.ifidx = m[46];
  e.data = eth + 72;
  if (e.dataLen > length - 72) e.dataLen = length - 72;
  if (e.type == kEvEscanResult) {
    onEscanResult(e);
    return;
  }
  static unsigned shown;
  if (shown++ < 64)
    printf("evb10: wifi: event %u status %u reason %u flags %x authtype %u data %u from %02x:%02x:%02x:%02x:%02x:%02x\n", e.type, e.status, e.reason,
           e.flags, e.authType, e.dataLen, e.addr[0], e.addr[1], e.addr[2], e.addr[3], e.addr[4], e.addr[5]);
  onConnEvent(e);
}

void onData(const uint8_t *eth, unsigned length);
void handleData(const uint8_t *eth, unsigned length) { onData(eth, length); }

// ---------------------------------------------------------------------------------------------------------------- scan

bool gScanV2;
uint8_t gMac[6];

// brcmf_run_escan: an "escan" iovar with one SSID entry (empty for all networks), active, all channels, the firmware's default dwell times.
int runEscan(const uint8_t *ssid, unsigned ssidLen) {
  uint8_t p[8 + 72 + 36];
  memset(p, 0, sizeof p);
  unsigned size, fixed, at;
  put32(p, gScanV2 ? 2 : 1);
  put16(p + 4, 1);       // WL_ESCAN_ACTION_START
  put16(p + 6, 0x1234);  // sync id
  if (gScanV2) {
    fixed = 72;
    put16(p + 8, 2);                                  // params version
    put16(p + 10, static_cast<uint16_t>(fixed + 36));  // params length
    put32(p + 12, 0);
    memset(p + 48, 0xff, 6);                          // any BSSID
    p[54] = 2;                                        // DOT11_BSSTYPE_ANY
    put32(p + 56, 0);                                 // active
    put32(p + 60, 0xffffffff);
    put32(p + 64, 0xffffffff);
    put32(p + 68, 0xffffffff);
    put32(p + 72, 0xffffffff);
    put32(p + 76, 1u << 16);                          // no channel list, one SSID entry
  } else {
    fixed = 64;
    memset(p + 44, 0xff, 6);
    p[50] = 2;
    p[51] = 0;
    put32(p + 52, 0xffffffff);
    put32(p + 56, 0xffffffff);
    put32(p + 60, 0xffffffff);
    put32(p + 64, 0xffffffff);
    put32(p + 68, 1u << 16);
  }
  at = 8 + fixed;  // the SSID entry follows the fixed part (no channels)
  if (ssidLen > 32) ssidLen = 32;
  put32(p + at, ssidLen);
  if (ssidLen) memcpy(p + at + 4, ssid, ssidLen);
  size = at + 36;
  return iovarSet("escan", p, size);
}

// ---------------------------------------------------------------------------------------------------------------- bring-up of the protocol

// What brcmf_c_preinit_dcmds and the first calls of cfg80211 do: version strings, the MAC address, no power saving in the radio, the
// events this driver reads, the scan dwell times.
bool dongleInit() {
  if (!f2Setup()) {
    printf("evb10: wifi: function 2 setup failed\n");
    return false;
  }
  const uint64_t began = nowUs();
  unsigned spins = 0;
  while (!gFwReady && nowUs() - began < 2000000) {
    if (!service()) idle(spins);
  }
  printf("evb10: wifi: firmware %s after %llu ms (frames in %u)\n", gFwReady ? "signalled ready" : "did not signal ready",
         static_cast<unsigned long long>((nowUs() - began) / 1000), gSdpcmStats[0]);
  uint8_t buf[260];
  memset(buf, 0, sizeof buf);
  int r = iovarGet("ver", buf, 256);
  if (r) {
    printf("evb10: wifi: the 'ver' query failed (%d; frames in %u, out %u, header errors %u, read errors %u, credit waits %u)\n", r, gSdpcmStats[0],
           gSdpcmStats[1], gSdpcmStats[5], gSdpcmStats[6], gSdpcmStats[7]);
    return false;
  }
  buf[255] = 0;
  for (unsigned i = 0; buf[i]; ++i)
    if (buf[i] == '\n') {
      buf[i] = ' ';
    }
  printf("evb10: wifi: firmware: %s\n", buf);
  r = iovarGet("cur_etheraddr", gMac, 6);
  printf("evb10: wifi: MAC address %02x:%02x:%02x:%02x:%02x:%02x (%d)\n", gMac[0], gMac[1], gMac[2], gMac[3], gMac[4], gMac[5], r);
  memset(buf, 0, sizeof buf);
  if (!iovarGet("clmver", buf, 128)) {
    buf[127] = 0;
    for (unsigned i = 0; buf[i]; ++i)
      if (buf[i] == '\n') buf[i] = ' ';
    printf("evb10: wifi: CLM %s\n", buf);
  }
  uint8_t sv[4] = {};
  gScanV2 = iovarGet("scan_ver", sv, 4) == 0;
  printf("evb10: wifi: scan parameters version %d\n", gScanV2 ? 2 : 1);
  r = iovarSetInt("bus:txglom", 0);   // no superframes from the chip (device to host)
  if (r) printf("evb10: wifi: bus:txglom %d\n", r);
  r = iovarSetInt("mpc", 0);
  if (r) printf("evb10: wifi: mpc %d\n", r);
  // The firmware passes only the multicast groups it was told about (brcmf_netdev_set_multicast_list); the network stack's IPv6 (neighbour
  // discovery, router advertisements) lives on them, so until it is told the chip drops all of it. The interface's multicast list is not
  // forwarded (SIOCADDMULTI is a no-op), so ask for every group.
  r = iovarSetInt("allmulti", 1);
  if (r) printf("evb10: wifi: allmulti %d\n", r);
  uint8_t mask[18] = {};
  r = iovarGet("event_msgs", mask, sizeof mask);
  if (r) printf("evb10: wifi: event_msgs get %d\n", r);
  static const uint8_t wanted[] = {kEvSetSsid, kEvJoin, kEvAuth, kEvDeauth, kEvDeauthInd, kEvAssoc, kEvDisassoc, kEvDisassocInd, kEvLink,
                                   kEvRoam, kEvPskSup, kEvIf, kEvEscanResult};
  for (uint8_t code : wanted) mask[code / 8] |= static_cast<uint8_t>(1u << (code % 8));
  r = iovarSet("event_msgs", mask, sizeof mask);
  if (r) printf("evb10: wifi: event_msgs set %d\n", r);
  cmdSetInt(kCmdSetScanChannelTime, 40);
  cmdSetInt(kCmdSetScanUnassocTime, 40);
  // A station in an infrastructure network (brcmf_cfg80211_change_iface): without WLC_SET_INFRA the firmware's default is an ad-hoc network,
  // and a "join" then only makes up a network of its own with a random BSSID.
  r = cmdSetInt(kCmdSetInfra, 1);
  const int ap = cmdSetInt(kCmdSetAp, 0);
  printf("evb10: wifi: infrastructure mode %d, not an access point %d\n", r, ap);
  // WLC_UP: until the radio is brought up every scan or join is refused with BCME_NOTUP (-4).
  r = dcmd(kCmdUp, true, nullptr, 0, nullptr, 0);
  printf("evb10: wifi: WLC_UP %d\n", r);
  return true;
}

// Scans with the given dwell, waits for the end, prints what was found.
void scanOnce(const char *what) {
  IOLockLock(gLock);
  gBssCount = 0;
  IOLockUnlock(gLock);
  gScanResults = 0;
  gScanFinished = false;
  const int r = runEscan(nullptr, 0);
  if (r) {
    printf("evb10: wifi: %s: the scan request failed (%d)\n", what, r);
    return;
  }
  gScanActive = true;
  const uint64_t began = nowUs();
  unsigned spins = 0;
  while (!gScanFinished && nowUs() - began < 15000000) {
    if (!service()) idle(spins);
  }
  printf("evb10: wifi: %s: scan %s after %llu ms: %u records, %u networks\n", what, gScanFinished ? "finished" : "TIMED OUT",
         static_cast<unsigned long long>((nowUs() - began) / 1000), gScanResults, gBssCount);
  for (unsigned i = 0; i < gBssCount && i < 40; ++i) {
    const nw_bss_info &b = gBss[i].info;
    printf("evb10: wifi:   %02x:%02x:%02x:%02x:%02x:%02x %u MHz %d dBm \"%.*s\"\n", b.bssid[0], b.bssid[1], b.bssid[2], b.bssid[3], b.bssid[4],
           b.bssid[5], b.freq_mhz, b.signal_dbm, b.ssid_len, reinterpret_cast<const char *>(b.ssid));
  }
}

// ---------------------------------------------------------------------------------------------------------------- station
//
// What the board's network code (nw_wifi_* below) asks for is only queued here; the worker thread does it. Events from the chip arrive in
// service() (which dcmd() also runs while it waits for its answer), so the handlers only change state and call the registered callbacks:
// no ioctl is sent from them.

constexpr unsigned kTxSlots = 128, kTxSlotBytes = 1600;
struct TxSlot {
  uint16_t length;
  uint8_t data[kTxSlotBytes];
};
TxSlot gTxQ[kTxSlots];
unsigned gTxIn, gTxOut;                                   // guarded by gLock
uint32_t gTxQueued, gTxDropped, gTxSent, gTxFailed, gRxDelivered, gRxDropped;

nw_wifi_events gEv;
nw_wifi_status gSt;                                       // guarded by gLock

bool gReqScan, gReqConnect, gReqDisconnect;                // guarded by gLock
uint8_t gReqSsid[32];
unsigned gReqSsidLen;
uint8_t gReqBssid[6];
bool gReqHasBssid;
char gReqPass[65];
unsigned gReqPassLen;

unsigned gLevel;   // evb10-wifi=N
bool gUserControl;  // the board's network code asked for a join or a leave: the join file steps aside
bool gConnecting, gPskJoin, gRefresh, gDisassoc, gReady;
uint64_t gConnectBegan, gScanBegan;
constexpr uint64_t kConnectTimeoutMs = 25000;

constexpr uint32_t kCmdDisassoc = 52, kCmdGetBssid = 23, kCmdGetRssi = 127, kCmdSetWsecPmk = 268, kCmdSetSsid = 26;

void setState(uint32_t state) {
  IOLockLock(gLock);
  gSt.state = state;
  IOLockUnlock(gLock);
}

void finishConnect(int result) {
  if (!gConnecting) return;
  gConnecting = false;
  IOLockLock(gLock);
  gSt.result = result;
  gSt.state = result == NW_RES_OK ? NW_STA_UP : NW_STA_IDLE;
  IOLockUnlock(gLock);
  if (result == NW_RES_OK) gRefresh = true;
  else gDisassoc = true;
  printf("evb10: wifi: join %s (result %d)\n", result == NW_RES_OK ? "succeeded" : "failed", result);
  if (gEv.connect_done) gEv.connect_done(result);
}

void linkLost(int reason) {
  IOLockLock(gLock);
  const bool wasUp = gSt.state == NW_STA_UP;
  if (wasUp) gSt.state = NW_STA_IDLE;
  IOLockUnlock(gLock);
  if (!wasUp) return;
  printf("evb10: wifi: link lost (reason %d)\n", reason);
  if (gEv.link_lost) gEv.link_lost(reason);
}

// Events that move the connection along (the chip's firmware supplicant does the 4-way handshake itself).
void onConnEvent(const Event &e) {
  switch (e.type) {
    case kEvAuth:
    case kEvAssoc:
      if (!gConnecting) break;
      if (e.status == 0) setState(NW_STA_ASSOC);
      else finishConnect(e.status == 2 ? NW_RES_TIMEOUT : NW_RES_REJECTED);
      break;
    case kEvSetSsid:
      if (!gConnecting) break;
      if (e.status != 0) finishConnect(e.status == 3 ? NW_RES_NOT_FOUND : e.status == 2 ? NW_RES_TIMEOUT : NW_RES_FAILED);
      else if (gPskJoin) setState(NW_STA_KEYS);
      else finishConnect(NW_RES_OK);
      break;
    case kEvPskSup:
      if (e.status == 6) finishConnect(NW_RES_OK);                       // WLC_SUP_KEYED
      else if (e.status == 7) finishConnect(NW_RES_BAD_KEY);             // WLC_SUP_KEYXCHANGE timeout: wrong passphrase
      break;
    case kEvLink:
      gLinkUp = (e.flags & 1) != 0;
      if (!gLinkUp) linkLost(static_cast<int>(e.reason));
      break;
    case kEvDeauth:
    case kEvDeauthInd:
    case kEvDisassoc:
    case kEvDisassocInd:
      linkLost(static_cast<int>(e.reason));
      break;
    default:
      break;
  }
}

// ---- self-test: DHCP, ARP and ping built here, to see the data path work before the board's network stack is on top of it (evb10-wifi=4)

bool gSelfTest;
uint8_t gStState;       // 0 wait for the link, 1 discover, 2 request, 3 bound, 4 arp, 5 ping, 6 done
uint32_t gStXid = 0x45564231;
uint32_t gOfferIp, gServerIp, gRouterIp, gDnsIp, gMask, gLease;
uint8_t gRouterMac[6];
bool gHaveRouterMac;
uint32_t gPingSent, gPingReplies, gPingTarget;
uint64_t gStStep, gPingSentAt;
uint32_t gStRtt;
uint8_t gStTries;

inline uint32_t be32v(const uint8_t *p) { return be32(p); }
inline void putbe16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
inline void putbe32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v & 0xff; }

uint16_t inetChecksum(const uint8_t *p, unsigned n) {
  uint32_t sum = 0;
  for (; n > 1; n -= 2, p += 2) sum += p[0] << 8 | p[1];
  if (n) sum += p[0] << 8;
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return static_cast<uint16_t>(~sum);
}

void printIp(const char *what, uint32_t ip) {
  printf("evb10: wifi:   %s %u.%u.%u.%u\n", what, ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
}

// Puts a frame in the transmit queue (the worker's loop sends it): the same path as nw_wifi_tx.
bool queueFrame(const uint8_t *frame, unsigned length) {
  if (length < 14 || length > kTxSlotBytes) return false;
  IOLockLock(gLock);
  const bool full = gTxIn - gTxOut >= kTxSlots;
  if (!full) {
    TxSlot &s = gTxQ[gTxIn % kTxSlots];
    s.length = static_cast<uint16_t>(length);
    memcpy(s.data, frame, length);
    gTxIn++;
    gTxQueued++;
  } else {
    gTxDropped++;
  }
  IOLockUnlock(gLock);
  return !full;
}

unsigned ethHeader(uint8_t *f, const uint8_t *dst, uint16_t type) {
  memcpy(f, dst, 6);
  memcpy(f + 6, gMac, 6);
  putbe16(f + 12, type);
  return 14;
}

unsigned ipHeader(uint8_t *f, uint8_t proto, uint32_t src, uint32_t dst, unsigned payload) {
  f[0] = 0x45;
  f[1] = 0;
  putbe16(f + 2, static_cast<uint16_t>(20 + payload));
  putbe16(f + 4, 0x1234);
  putbe16(f + 6, 0);
  f[8] = 64;
  f[9] = proto;
  putbe16(f + 10, 0);
  putbe32(f + 12, src);
  putbe32(f + 16, dst);
  putbe16(f + 10, inetChecksum(f, 20));
  return 20;
}

void sendDhcp(uint8_t type) {
  static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  uint8_t f[14 + 20 + 8 + 300];
  memset(f, 0, sizeof f);
  uint8_t *d = f + 14 + 20 + 8;
  d[0] = 1;
  d[1] = 1;
  d[2] = 6;
  putbe32(d + 4, gStXid);
  putbe16(d + 10, 0x8000);  // broadcast the answer
  memcpy(d + 28, gMac, 6);
  d[236] = 0x63;
  d[237] = 0x82;
  d[238] = 0x53;
  d[239] = 0x63;
  unsigned o = 240;
  d[o++] = 53;
  d[o++] = 1;
  d[o++] = type;
  if (type == 3) {
    d[o++] = 50;
    d[o++] = 4;
    putbe32(d + o, gOfferIp);
    o += 4;
    d[o++] = 54;
    d[o++] = 4;
    putbe32(d + o, gServerIp);
    o += 4;
  }
  d[o++] = 55;
  d[o++] = 3;
  d[o++] = 1;
  d[o++] = 3;
  d[o++] = 6;
  d[o++] = 255;
  const unsigned dl = (o + 3) & ~3u;
  unsigned at = ethHeader(f, bcast, 0x0800);
  at += ipHeader(f + at, 17, 0, 0xffffffff, 8 + dl);
  putbe16(f + at, 68);
  putbe16(f + at + 2, 67);
  putbe16(f + at + 4, static_cast<uint16_t>(8 + dl));
  putbe16(f + at + 6, 0);
  queueFrame(f, at + 8 + dl);
}

void sendArp(uint16_t op, const uint8_t *targetMac, uint32_t targetIp, const uint8_t *dstMac) {
  uint8_t f[14 + 28];
  memset(f, 0, sizeof f);
  unsigned at = ethHeader(f, dstMac, 0x0806);
  putbe16(f + at, 1);
  putbe16(f + at + 2, 0x0800);
  f[at + 4] = 6;
  f[at + 5] = 4;
  putbe16(f + at + 6, op);
  memcpy(f + at + 8, gMac, 6);
  putbe32(f + at + 14, gOfferIp);
  if (targetMac) memcpy(f + at + 18, targetMac, 6);
  putbe32(f + at + 24, targetIp);
  queueFrame(f, at + 28);
}

void sendPing(uint32_t target, uint16_t seq) {
  uint8_t f[14 + 20 + 8 + 32];
  memset(f, 0, sizeof f);
  unsigned at = ethHeader(f, gRouterMac, 0x0800);   // everything goes through the router, which also answers for its own address
  at += ipHeader(f + at, 1, gOfferIp, target, 8 + 32);
  f[at] = 8;
  putbe16(f + at + 4, 0x4556);
  putbe16(f + at + 6, seq);
  for (unsigned i = 0; i < 32; ++i) f[at + 8 + i] = static_cast<uint8_t>('a' + i % 26);
  putbe16(f + at + 2, inetChecksum(f + at, 8 + 32));
  queueFrame(f, at + 8 + 32);
}

// The frames the test waits for; called from service() with every data frame.
void selfTestRx(const uint8_t *f, unsigned n) {
  if (n < 14) return;
  const unsigned type = f[12] << 8 | f[13];
  if (type == 0x0806 && n >= 42) {
    const unsigned op = f[20] << 8 | f[21];
    const uint32_t spa = be32v(f + 28), tpa = be32v(f + 38);
    if (op == 2 && gStState == 4 && spa == gRouterIp) {
      memcpy(gRouterMac, f + 22, 6);
      gHaveRouterMac = true;
    } else if (op == 1 && gOfferIp && tpa == gOfferIp) {
      sendArp(2, f + 22, spa, f + 22);   // answer the router's question about us
    }
    return;
  }
  if (type != 0x0800 || n < 14 + 20) return;
  const unsigned ihl = (f[14] & 15) * 4;
  const uint8_t proto = f[14 + 9];
  if (n < 14 + ihl + 8) return;
  const uint8_t *l4 = f + 14 + ihl;
  if (proto == 1 && l4[0] == 0 && l4[4] == 0x45 && l4[5] == 0x56 && be32v(f + 14 + 12) == gPingTarget) {
    gPingReplies++;
    gStRtt = static_cast<uint32_t>(nowMs() - gPingSentAt);
    return;
  }
  if (proto != 17 || (l4[2] << 8 | l4[3]) != 68 || n < 14 + ihl + 8 + 240) return;
  const uint8_t *d = l4 + 8;
  if (d[0] != 2 || be32v(d + 4) != gStXid || memcmp(d + 28, gMac, 6)) return;
  uint8_t msg = 0;
  uint32_t router = 0, dns = 0, mask = 0, server = 0, lease = 0;
  for (unsigned o = 240; o + 1 < n - (14 + ihl + 8);) {
    const uint8_t code = d[o];
    if (code == 255) break;
    if (code == 0) {
      o++;
      continue;
    }
    const uint8_t len = d[o + 1];
    const uint8_t *v = d + o + 2;
    if (o + 2 + len > n - (14 + ihl + 8)) break;
    if (code == 53 && len == 1) msg = v[0];
    if (code == 1 && len == 4) mask = be32v(v);
    if (code == 3 && len >= 4) router = be32v(v);
    if (code == 6 && len >= 4) dns = be32v(v);
    if (code == 54 && len == 4) server = be32v(v);
    if (code == 51 && len == 4) lease = be32v(v);
    o += 2 + len;
  }
  if (msg == 2 && gStState == 1) {
    gOfferIp = be32v(d + 16);
    gServerIp = server;
    gStState = 2;
    gStStep = 0;
    gStTries = 0;
  } else if (msg == 5 && gStState == 2) {
    gOfferIp = be32v(d + 16);
    gRouterIp = router;
    gDnsIp = dns;
    gMask = mask;
    gLease = lease;
    gStState = 3;
  } else if (msg == 6) {
    gStState = 1;
    gStStep = 0;
  }
}

// One step of the self-test, from the worker's loop.
bool selfTestStep() {
  bool up;
  IOLockLock(gLock);
  up = gSt.state == NW_STA_UP;
  IOLockUnlock(gLock);
  if (!up) {
    gStState = 0;
    return false;
  }
  const uint64_t now = nowMs();
  if (gStState == 0) {
    gStState = 1;
    gStStep = 0;
    gStTries = 0;
    gOfferIp = gServerIp = gRouterIp = gDnsIp = gMask = 0;
    gHaveRouterMac = false;
    gPingSent = gPingReplies = 0;
    printf("evb10: wifi: self-test: link is up, asking for an address (DHCP)\n");
  }
  if (gStState == 1 || gStState == 2) {
    if (gStStep == 0 || now - gStStep > 3000) {
      if (gStTries++ >= 6) {
        printf("evb10: wifi: self-test: no DHCP answer (state %u): sent %u frames, received %u\n", gStState, gTxSent, gRxDelivered);
        gStState = 6;
        return false;
      }
      gStStep = now;
      sendDhcp(gStState == 1 ? 1 : 3);
    }
    return false;
  }
  if (gStState == 3) {
    printf("evb10: wifi: self-test: DHCP bound, lease %u s\n", gLease);
    printIp("address", gOfferIp);
    printIp("mask", gMask);
    printIp("router", gRouterIp);
    printIp("dns", gDnsIp);
    gStState = 4;
    gStStep = 0;
    gStTries = 0;
  }
  if (gStState == 4) {
    if (gHaveRouterMac) {
      printf("evb10: wifi: self-test: router at %02x:%02x:%02x:%02x:%02x:%02x\n", gRouterMac[0], gRouterMac[1], gRouterMac[2], gRouterMac[3],
             gRouterMac[4], gRouterMac[5]);
      gStState = 5;
      gStStep = 0;
      gPingTarget = gRouterIp;
      return false;
    }
    if (gStStep == 0 || now - gStStep > 1000) {
      if (gStTries++ >= 5) {
        printf("evb10: wifi: self-test: the router did not answer ARP\n");
        gStState = 6;
        return false;
      }
      gStStep = now;
      static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
      sendArp(1, nullptr, gRouterIp, bcast);
    }
    return false;
  }
  if (gStState == 5) {
    if (gStStep == 0 || now - gStStep > 1000) {
      if (gStStep) {
        printf("evb10: wifi: self-test: ping %u.%u.%u.%u #%u: %s\n", gPingTarget >> 24, (gPingTarget >> 16) & 0xff, (gPingTarget >> 8) & 0xff,
               gPingTarget & 0xff, gPingSent, gPingReplies >= gPingSent ? "reply" : "no reply");
        if (gPingSent == 4) {
          if (gPingTarget == gRouterIp && gDnsIp && gDnsIp != gRouterIp) {
            gPingTarget = gDnsIp;
            gPingSent = 0;
          } else {
            printf("evb10: wifi: self-test: finished (frames sent %u failed %u dropped %u, received %u)\n", gTxSent, gTxFailed, gTxDropped, gRxDelivered);
            gStState = 6;
            return false;
          }
        }
      }
      gStStep = now;
      gPingSentAt = now;
      sendPing(gPingTarget, static_cast<uint16_t>(++gPingSent));
    }
  }
  return false;
}

// ---- the worker

// One queued frame to the chip: BCDC data header (version 2, priority, interface 0) and the Ethernet frame, on channel 2.
bool txPump() {
  bool any = false;
  for (unsigned n = 0; n < 8; ++n) {
    if (!creditOk(1)) break;
    uint8_t buf[4 + kTxSlotBytes];
    unsigned length = 0;
    IOLockLock(gLock);
    if (gTxIn != gTxOut) {
      const TxSlot &s = gTxQ[gTxOut % kTxSlots];
      length = s.length;
      memcpy(buf + 4, s.data, length);
      gTxOut++;
    }
    IOLockUnlock(gLock);
    if (!length) break;
    buf[0] = 0x20;
    buf[1] = 0;
    buf[2] = 0;
    buf[3] = 0;
    if (sdpcmSend(2, buf, 4 + length)) gTxFailed++;
    else gTxSent++;
    any = true;
  }
  return any;
}

void onData(const uint8_t *eth, unsigned length) {
  if (gSelfTest) selfTestRx(eth, length);
  IOLockLock(gLock);
  const bool up = gSt.state == NW_STA_UP;
  IOLockUnlock(gLock);
  if (up && gEv.rx) {
    gRxDelivered++;
    gEv.rx(eth, length);
  } else {
    gRxDropped++;
  }
}

// What the join needs from the firmware: how to authenticate, the cipher, the key; then the join itself (brcmf_cfg80211_connect).
void doConnect(const uint8_t *ssid, unsigned ssidLen, const uint8_t *bssid, bool hasBssid, const char *pass, unsigned passLen) {
  const bool psk = passLen != 0;
  int r = cmdSetInt(kCmdSetInfra, 1);
  if (!r) r = bsscfgSetInt("wpa_auth", psk ? 0x80 : 0);
  if (!r) r = bsscfgSetInt("auth", 0);
  if (!r) r = bsscfgSetInt("wsec", psk ? 4 : 0);
  if (!r) r = iovarSetInt("sup_wpa", psk ? 1 : 0);
  if (r) {
    printf("evb10: wifi: join setup failed (%d)\n", r);
    return;
  }
  if (psk) {
    uint8_t k[4 + 128];
    memset(k, 0, sizeof k);
    put16(k, static_cast<uint16_t>(passLen));
    put16(k + 2, 1);                    // BRCMF_WSEC_PASSPHRASE: the firmware makes the PMK
    memcpy(k + 4, pass, passLen);
    r = dcmd(kCmdSetWsecPmk, true, k, sizeof k, nullptr, 0);
    if (r) {
      printf("evb10: wifi: setting the passphrase failed (%d)\n", r);
      return;
    }
  }
  // brcmf_ext_join_params_le with natural alignment: ssid (36), scan (type at 36, nprobes 40, active 44, passive 48, home 52), assoc
  // (bssid at 56, chanspec count at 64), no channel list.
  uint8_t j[68];
  memset(j, 0, sizeof j);
  put32(j, ssidLen);
  memcpy(j + 4, ssid, ssidLen);
  j[36] = 0xff;
  put32(j + 40, 0xffffffff);
  put32(j + 44, 0xffffffff);
  put32(j + 48, 0xffffffff);
  put32(j + 52, 0xffffffff);
  if (hasBssid) memcpy(j + 56, bssid, 6);
  else memset(j + 56, 0xff, 6);
  gPskJoin = psk;
  gConnecting = true;
  gConnectBegan = nowMs();
  IOLockLock(gLock);
  gSt.state = NW_STA_AUTH;
  gSt.result = 0;
  gSt.ssid_len = static_cast<uint8_t>(ssidLen);
  memset(gSt.ssid, 0, sizeof gSt.ssid);
  memcpy(gSt.ssid, ssid, ssidLen);
  IOLockUnlock(gLock);
  r = bsscfgSet("join", j, sizeof j);
  if (r) {
    printf("evb10: wifi: the join command failed (%d), trying WLC_SET_SSID\n", r);
    r = dcmd(kCmdSetSsid, true, j, 36, nullptr, 0);
  }
  if (r) {
    printf("evb10: wifi: WLC_SET_SSID failed (%d)\n", r);
    finishConnect(NW_RES_FAILED);
    return;
  }
  printf("evb10: wifi: joining \"%.*s\"%s\n", ssidLen, reinterpret_cast<const char *>(ssid), psk ? " (WPA2-PSK, firmware supplicant)" : " (open)");
}

// What the firmware thinks of the join, when it does not finish: security settings, the association request it sent and the console.
void hexLine(const char *what, const uint8_t *p, unsigned n) {
  char line[140];
  unsigned at = snprintf(line, sizeof line, "evb10: wifi: %s (%u):", what, n);
  for (unsigned i = 0; i < n && at + 4 < sizeof line; ++i) at += snprintf(line + at, sizeof line - at, " %02x", p[i]);
  printf("%s\n", line);
}

void diagnose() {
  uint8_t v[4];
  int r;
  static const char *const names[] = {"bsscfg:wsec", "bsscfg:wpa_auth", "bsscfg:auth", "sup_wpa", "bsscfg:sup_wpa", "sup_auth_status", "bsscfg:sup_auth_status",
                                      "sup_wpa_tmo", "mfp", "bsscfg:mfp"};
  for (const char *n : names) {
    memset(v, 0, 4);
    r = iovarGet(n, v, 4);
    printf("evb10: wifi: diag %s = %d (rc %d)\n", n, static_cast<int32_t>(le32(v)), r);
  }
  uint8_t info[64] = {};
  r = iovarGet("assoc_info", info, 64);
  hexLine("assoc_info", info, 24);
  if (!r) {
    const uint32_t reqLen = le32(info), respLen = le32(info + 4);
    uint8_t ies[256] = {};
    r = iovarGet("assoc_req_ies", ies, sizeof ies);
    hexLine("assoc request IEs", ies, reqLen < 120 ? reqLen : 120);
    memset(ies, 0, sizeof ies);
    r = iovarGet("assoc_resp_ies", ies, sizeof ies);
    hexLine("assoc response IEs", ies, respLen < 80 ? respLen : 80);
  }
  dumpConsole();
}

// After a join: the access point, signal and channel, from the chip.
void refreshStatus() {
  uint8_t bssid[6] = {};
  uint8_t zero[12] = {};
  uint8_t rssi[12] = {};
  uint8_t cs[4] = {};
  const int a = dcmd(kCmdGetBssid, false, zero, 6, bssid, 6);
  const int b = dcmd(kCmdGetRssi, false, zero, 12, rssi, 12);
  const int c = iovarGet("chanspec", cs, 4);
  IOLockLock(gLock);
  if (!a) memcpy(gSt.bssid, bssid, 6);
  if (!b) gSt.signal_dbm = static_cast<int8_t>(static_cast<int32_t>(le32(rssi)));
  if (!c) {
    const uint32_t spec = le32(cs);
    const unsigned channel = spec & 0xff;
    gSt.freq_mhz = (spec & 0xc000) == 0xc000 ? 5000 + 5 * channel : (channel == 14 ? 2484 : 2407 + 5 * channel);
  }
  IOLockUnlock(gLock);
  printf("evb10: wifi: connected to %02x:%02x:%02x:%02x:%02x:%02x on %u MHz, %d dBm\n", gSt.bssid[0], gSt.bssid[1], gSt.bssid[2], gSt.bssid[3],
         gSt.bssid[4], gSt.bssid[5], gSt.freq_mhz, gSt.signal_dbm);
}

bool handleRequests() {
  uint8_t ssid[32], bssid[6];
  char pass[65];
  unsigned ssidLen = 0, passLen = 0;
  bool scan, connect, disconnect, hasBssid = false;
  IOLockLock(gLock);
  scan = gReqScan;
  connect = gReqConnect;
  disconnect = gReqDisconnect;
  gReqScan = gReqConnect = gReqDisconnect = false;
  if (scan || connect) {
    ssidLen = gReqSsidLen;
    memcpy(ssid, gReqSsid, sizeof ssid);
    memcpy(bssid, gReqBssid, 6);
    hasBssid = gReqHasBssid;
    passLen = gReqPassLen;
    memcpy(pass, gReqPass, sizeof pass);
  }
  IOLockUnlock(gLock);
  bool any = false;
  if (disconnect || gDisassoc) {
    gDisassoc = false;
    const int r = dcmd(kCmdDisassoc, true, nullptr, 0, nullptr, 0);
    if (disconnect) {
      gConnecting = false;
      setState(NW_STA_IDLE);
    }
    printf("evb10: wifi: disassociate %d\n", r);
    any = true;
  }
  if (connect) {
    doConnect(ssid, ssidLen, bssid, hasBssid, pass, passLen);
    any = true;
  } else if (scan && !gScanActive) {
    gScanFinished = false;
    gScanResults = 0;
    const int r = runEscan(ssidLen ? ssid : nullptr, ssidLen);
    if (r) {
      printf("evb10: wifi: scan request failed (%d)\n", r);
      if (gEv.scan_done) gEv.scan_done();
    } else {
      gScanActive = true;
      gScanBegan = nowMs();
    }
    any = true;
  }
  if (gRefresh) {
    gRefresh = false;
    refreshStatus();
    any = true;
  }
  return any;
}

bool timers() {
  const uint64_t now = nowMs();
  static uint64_t diagFor;
  if (gConnecting && now - gConnectBegan > 8000 && diagFor != gConnectBegan) {
    diagFor = gConnectBegan;
    diagnose();
  }
  if (gConnecting && now - gConnectBegan > kConnectTimeoutMs) finishConnect(NW_RES_TIMEOUT);
  if (gScanActive && now - gScanBegan > 15000) {
    gScanActive = false;
    gScanFinished = true;
  }
  if (gScanFinished) {
    gScanFinished = false;
    gScanActive = false;
    if (gEv.scan_done) gEv.scan_done();
  }
  return false;
}

// The join file (.cache/firmware/join.txt, embedded): the SSID on the first line, the passphrase on the second.
bool joinFile(uint8_t *ssid, unsigned *ssidLen, char *pass, unsigned *passLen) {
  if (!kWifiJoinSize) return false;
  unsigned i = 0, n = 0;
  while (i < kWifiJoinSize && kWifiJoin[i] != '\n' && kWifiJoin[i] != '\r' && n < 32) ssid[n++] = kWifiJoin[i++];
  while (i < kWifiJoinSize && (kWifiJoin[i] == '\n' || kWifiJoin[i] == '\r')) i++;
  *ssidLen = n;
  n = 0;
  while (i < kWifiJoinSize && kWifiJoin[i] != '\n' && kWifiJoin[i] != '\r' && n < 64) pass[n++] = static_cast<char>(kWifiJoin[i++]);
  pass[n] = 0;
  *passLen = n;
  return *ssidLen != 0;
}

// The interface and the registry entries: the static initializer of WifiNet.cpp runs between preModLoad and postModLoad.
void startNet() {
  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (!handle) {
    printf("evb10: wifi: preModLoad failed\n");
    return;
  }
  evb10_wifi_net_static_init();
  const int r = OSMetaClass::postModLoad(handle);
  if (r) {
    printf("evb10: wifi: postModLoad %d\n", r);
    return;
  }
  evb10_wifi_net_start(gMac);
}

void stationMain() {
  if (!dongleInit()) return;
  gReady = true;
  evb10_wifi_stats[0] = 9;
  setState(NW_STA_IDLE);
  IOLockLock(gLock);
  memcpy(gSt.mac, gMac, 6);
  IOLockUnlock(gLock);
  if (gLevel == 3)
    for (unsigned round = 0; round < 3; ++round) scanOnce(round ? "rescan" : "first scan");
  uint8_t ssid[32];
  char pass[65];
  unsigned ssidLen = 0, passLen = 0;
  // evb10-wifi-join=0: leave the join file alone, so that the network is joined from the OS (Settings) and not at boot
  unsigned useFile = 1;
  PE_parse_boot_argn("evb10-wifi-join", &useFile, sizeof useFile);
  const bool haveJoin = gLevel >= 4 && useFile == 1 && joinFile(ssid, &ssidLen, pass, &passLen);
  if (useFile == 2) {
    uint8_t onlySsid[32];
    char onlyPass[65];
    unsigned onlyLen = 0, onlyPassLen = 0;
    if (joinFile(onlySsid, &onlyLen, onlyPass, &onlyPassLen)) {
      memcpy(gOnlySsid, onlySsid, sizeof gOnlySsid);
      gOnlySsidLen = onlyLen;
    }
    memset(onlyPass, 0, sizeof onlyPass);
  }
  uint64_t lastAlive = nowMs(), lastTry = 0;
  unsigned tries = 0;
  gSelfTest = gLevel == 4;
  if (gLevel >= 5) startNet();
  unsigned spins = 0;
  for (;;) {
    bool busy = service();
    busy |= txPump();
    busy |= handleRequests();
    busy |= timers();
    if (gSelfTest) busy |= selfTestStep();
    const uint64_t now = nowMs();
    if (haveJoin && !gUserControl && now - lastTry > 30000 && !gConnecting) {
      bool up;
      IOLockLock(gLock);
      up = gSt.state == NW_STA_UP;
      IOLockUnlock(gLock);
      if (!up && tries < 10) {
        lastTry = now;
        tries++;
        printf("evb10: wifi: join attempt %u of the file's network\n", tries);
        doConnect(ssid, ssidLen, nullptr, false, pass, passLen);
      }
    }
    if (busy) spins = 0;
    else idle(spins);
    if (now - lastAlive > 30000) {
      lastAlive = now;
      printf("evb10: wifi: alive: frames in %u out %u, events %u, data %u, header errors %u, read errors %u, link %s, tx sent %u/failed %u, rx delivered %u\n",
             gSdpcmStats[0], gSdpcmStats[1], gSdpcmStats[3], gSdpcmStats[4], gSdpcmStats[5], gSdpcmStats[6], gLinkUp ? "up" : "down", gTxSent,
             gTxFailed, gRxDelivered);
    }
  }
}

thread_call_t gCall;

// Everything up to a running firmware (and, from level 2, the check that it runs).
bool bringUp() {
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
    return false;
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
  if (!ok || gLevel < 2) return false;
  if (!busSwitch()) {
    printf("evb10: wifi: switching to 4 bits and 25 MHz failed\n");
    return false;
  }
  evb10_wifi_stats[0] = 6;
  printf("evb10: wifi: bus at 4 bits, about 25 MHz (CRU sel172 %08x)\n", rd(gCru, 0x5b0));
  if (!downloadAndRun()) {
    printf("evb10: wifi: firmware start failed (steps reached: %u)\n", evb10_wifi_stats[0]);
    return false;
  }
  printf("evb10: wifi: the AP6398S firmware runs\n");
  return true;
}

void workerMain(void *, wait_result_t) {
  if (bringUp() && gLevel >= 3) stationMain();
  thread_terminate(current_thread());
}

void start(thread_call_param_t, thread_call_param_t) {
  thread_t t = nullptr;
  if (kernel_thread_start(workerMain, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
  else printf("evb10: wifi: no worker thread\n");
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- station interface (nw_abi.h)
//
// The calls the board's network code makes (package/Evb10Wifi/WifiNet.cpp, adapted from the sheng port): each queues a request or copies a
// snapshot and returns; the worker thread above does the rest and calls the registered events.

extern "C" {

void nw_wifi_set_events(const struct nw_wifi_events *ev) {
  if (ev) gEv = *ev;
  else memset(&gEv, 0, sizeof gEv);
}

int nw_wifi_scan(const uint8_t *ssid, size_t len) {
  if (!gReady || !gLock || len > 32) return -1;
  IOLockLock(gLock);
  gReqSsidLen = ssid ? static_cast<unsigned>(len) : 0;
  memset(gReqSsid, 0, sizeof gReqSsid);
  if (ssid && len) memcpy(gReqSsid, ssid, len);
  gReqScan = true;
  IOLockUnlock(gLock);
  return 0;
}

int nw_wifi_bss_count(void) {
  if (!gLock) return 0;
  IOLockLock(gLock);
  const int n = static_cast<int>(gBssCount);
  IOLockUnlock(gLock);
  return n;
}

int nw_wifi_bss_get(int i, struct nw_bss_info *out) {
  if (!gLock || !out || i < 0) return -1;
  int r = -1;
  IOLockLock(gLock);
  if (static_cast<unsigned>(i) < gBssCount) {
    *out = gBss[i].info;
    const uint64_t age = nowMs() - gBss[i].seenMs;
    out->age_ms = age > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(age);
    r = 0;
  }
  IOLockUnlock(gLock);
  return r;
}

int nw_wifi_connect(const uint8_t *ssid, size_t len, const uint8_t *bssid, const char *pass, size_t plen) {
  if (!gReady || !gLock || !ssid || len == 0 || len > 32 || plen > 64 || (pass && plen && plen < 8)) return -1;
  IOLockLock(gLock);
  gUserControl = true;
  gReqSsidLen = static_cast<unsigned>(len);
  memset(gReqSsid, 0, sizeof gReqSsid);
  memcpy(gReqSsid, ssid, len);
  gReqHasBssid = bssid != nullptr;
  if (bssid) memcpy(gReqBssid, bssid, 6);
  gReqPassLen = pass ? static_cast<unsigned>(plen) : 0;
  memset(gReqPass, 0, sizeof gReqPass);
  if (pass && plen) memcpy(gReqPass, pass, plen);
  gReqConnect = true;
  IOLockUnlock(gLock);
  return 0;
}

void nw_wifi_disconnect(void) {
  if (!gLock) return;
  IOLockLock(gLock);
  gUserControl = true;
  gReqDisconnect = true;
  IOLockUnlock(gLock);
}

int nw_wifi_tx(const uint8_t *eth, size_t len) {
  if (!gReady || !gLock || !eth || len < 14 || len > kTxSlotBytes) return -1;
  return queueFrame(eth, static_cast<unsigned>(len)) ? 0 : -2;
}

void nw_wifi_status_get(struct nw_wifi_status *st) {
  if (!st) return;
  memset(st, 0, sizeof *st);
  if (!gLock) return;
  IOLockLock(gLock);
  *st = gSt;
  IOLockUnlock(gLock);
  if (!gReady) st->state = NW_STA_OFF;
  else if (st->state == NW_STA_IDLE && gScanActive) st->state = NW_STA_SCANNING;
}

void nw_wifi_tx_reasons(char *text, int size) {
  if (!text || size <= 0) return;
  snprintf(text, static_cast<size_t>(size), "queued %u sent %u failed %u dropped %u, rx delivered %u dropped %u", gTxQueued, gTxSent, gTxFailed,
           gTxDropped, gRxDelivered, gRxDropped);
}

}  // extern "C"

// Called by the display probe once the HID services are up (package/LegacyDisplayProbe.cpp).
extern "C" void evb10_wifi_start(void) {
  unsigned on = 0;
  if (!PE_parse_boot_argn("evb10-wifi", &on, sizeof on) || (on < 1 || on > 5) || gCall) return;
  gLevel = on;
  gLock = IOLockAlloc();
  gCall = thread_call_allocate(start, nullptr);
  if (!gCall) {
    printf("evb10: wifi: no thread call\n");
    return;
  }
  thread_call_enter(gCall);
}
