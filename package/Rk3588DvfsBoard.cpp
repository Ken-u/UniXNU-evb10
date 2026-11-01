// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The regulators of the EVB10's CPU clusters and GPU, for UniXNU-rk3588-common's frequency management (scripts/data/Rk3588Dvfs/Rk3588Dvfs.h).
// Wiring and limits are those of the board's device tree (kernel-6.12 rk3588-evb10.dtsi, rk3588-rk806-single.dtsi) and the firmware's
// platform library (uefi-evb10 UniXNUEvb10/Library/RockchipPlatformLib):
//   vdd_cpu_lit_s0   RK806 DCDC_REG2 on SPI2    550..950 mV, ramp 12.5 mV/us   the four Cortex-A55 cores and the DSU
//   vdd_cpu_big0_s0  RK8602 0x42 on I2C0        550..1050 mV, ramp 2.3 mV/us   the Cortex-A76 cores 0 and 1
//   vdd_cpu_big1_s0  RK8603 0x43 on I2C0        550..1050 mV, ramp 2.3 mV/us   the Cortex-A76 cores 2 and 3
//   vdd_gpu_s0       RK806 DCDC_REG1 on SPI2    550..950 mV, ramp 12.5 mV/us   the Mali-G610
#include "Rk3588Dvfs.h"

// The touch driver's count of the reports it hands to the HID stack (package/Evb10Touch); weak, for a kernel built without it.
extern "C" volatile uint32_t native_touch_hid_reports __attribute__((weak_import));

extern "C" {
const Rk3588DvfsRail rk3588_dvfs_rails[] = {
    {kRk3588ScmiClkCpuL, kRk3588DvfsRk806, 2, 0, 550000, 950000, 12500},
    {kRk3588ScmiClkCpuB01, kRk3588DvfsRk860x, 0, 0x42, 550000, 1050000, 2300},
    {kRk3588ScmiClkCpuB23, kRk3588DvfsRk860x, 0, 0x43, 550000, 1050000, 2300},
    {kRk3588ScmiClkGpu, kRk3588DvfsRk806, 1, 0, 550000, 950000, 12500},
};
const unsigned rk3588_dvfs_rail_count = sizeof rk3588_dvfs_rails / sizeof rk3588_dvfs_rails[0];

unsigned rk3588_dvfs_activity(void) { return &native_touch_hid_reports ? native_touch_hid_reports : 0; }
}
