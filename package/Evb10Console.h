// SPDX-License-Identifier: BSD-2-Clause-Patent
// The console the display drivers see, with the kernel's own frame buffer in place of the firmware's (evb10-fb-w/-h).
//
// PE_state is SECURITY_READ_ONLY_LATE: once the kernel has locked down, a store to PE_state.video faults ("Unexpected fault in kernel
// static region", touch1). The replacement surface is therefore kept in plain kernel globals (defined next to evb10_vop_show_gop in
// patches/tools/evb10-userland-mounts.c.inc, zero while there is none) and every PE_current_console of the display
// side returns it instead of PE_state.video. The display drivers of UniXNU (drivers/GopFramebuffer: NativeLegacyDisplay,
// NativeGOPPresenter) call PE_current_console to compare the console with their device memory (touch2: a 1080x1920 nub against the
// firmware's 640x480 console made NativeLegacyDisplay::startHardware refuse), so scripts/build-arm-driver.py force-includes this
// header into them, and package/LegacyDisplayProbe.cpp includes it.
#pragma once
#include <pexpert/pexpert.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern uint64_t evb10_fb_base;
extern uint32_t evb10_fb_w, evb10_fb_h, evb10_fb_row;
extern uintptr_t evb10_fb_kva;   // the kernel's own mapping of the surface
#ifdef __cplusplus
}
#endif
static inline int evb10_current_console(PE_Video *info)
{
	const int result = PE_current_console(info);
	if (!result && evb10_fb_base) {
		info->v_baseAddr = evb10_fb_base;
		info->v_rowBytes = evb10_fb_row;
		info->v_width = evb10_fb_w;
		info->v_height = evb10_fb_h;
		info->v_offset = 0;
		info->v_length = 0;
	}
	return result;
}
#define PE_current_console evb10_current_console
