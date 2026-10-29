// SPDX-License-Identifier: BSD-2-Clause-Patent
// No-op definitions of hooks that the display archive references from sheng's boot progress screen, which this board
// does not have. Derived from LinkStubs.c of the community UniXNU-qemu repository (BSD-2-Clause-Patent, Copyright (c)
// 2026, UniXNU contributors), reduced to what the EVB10 display archive needs.
//
// Each definition behaves as the absent hook: the display may always draw, keeps no diagnostic mapping and reports no
// stages. They are weak so that a linked component which defines a hook replaces it.

#include <stdint.h>

#define STUB __attribute__((weak))

int boot_progress_prepare_scanout(void);
STUB int boot_progress_prepare_scanout(void)
{
  return 1;
}

void boot_progress_cancel_scanout(void);
STUB void boot_progress_cancel_scanout(void)
{
}

void boot_progress_stage(const char *message);
STUB void boot_progress_stage(const char *message)
{
  (void)message;
}

// The extension rows of sheng's diagnostic page, where KeyStoreProbe.cpp writes its AppleKeyStore counters.
STUB char boot_progress_ext[8][48];

int boot_progress_pulse_anchor(uintptr_t address, uint64_t bytes, unsigned width, unsigned height, unsigned row);
STUB int boot_progress_pulse_anchor(uintptr_t address, uint64_t bytes, unsigned width, unsigned height, unsigned row)
{
  (void)address;
  (void)bytes;
  (void)width;
  (void)height;
  (void)row;
  return 0;
}

// Qualcomm direct-scanout path of NativeLegacyDisplay (sheng-dpu-scanout, needs the phone's DPU SMMU). It never starts
// without a live state word (bit 31) and a mapped surface, so these stay inert on a Rockchip board.
STUB volatile uint32_t native_ufs_dpu_state;

uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages, unsigned count);
STUB uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages, unsigned count)
{
  (void)slot;
  (void)pages;
  (void)count;
  return 0;
}
