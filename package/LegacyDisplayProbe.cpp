// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded offline original lifecycle instrument; no desktop claim.
// LegacyDisplayProbe.cpp of UniXNU-sheng, as the community UniXNU-qemu repository cut it (BSD-2-Clause-Patent,
// Copyright (c) 2026, UniXNU contributors), cut once more for the RK3588 EVB10: the late probe starts the key store
// stand-in and then publishes the UEFI frame buffer as the system display. QEMU's VirtIO input, shared memory window and
// Metal accelerator probes are gone (this board has none of them; its HID input and GPU are later steps), as are
// sheng's CPU frequency, GPU power and touch panel probes, which drive sheng hardware.
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSData.h>
#include <kern/task.h>
#include <sys/proc.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include <machine/machine_routines.h>
#include <libkern/c++/OSKext.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
// evb10-fb-w/-h: the kernel's own frame buffer; PE_current_console of this file returns it (Evb10Console.h says why PE_state is not written).
#include "Evb10Console.h"
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void native_legacy_display_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" void native_gop_presenter_static_init(void) __asm__("__GLOBAL__sub_I_NativeGOPPresenter.cpp");
extern "C" IOService *native_gop_presenter_create(void);
static IOService *pendingLegacyDisplay;
static IOService *activeGOPPresenter,*activeGOPNub;
// GPU hand-off presenter (sheng-fb-gpu=1, phase 4): lives beside the display driver, which offers it frames instead of copying them.
static IOService *gpuHandoffPresenter;
extern "C" volatile uint64_t native_gop_gpu_offers,native_gop_gpu_completions,native_gop_gpu_failures,native_gop_gpu_abandoned,native_gop_wait_stats[7];
extern "C" volatile uint64_t native_legacy_early_retires,native_legacy_early_failures;
extern "C" bool native_gop_gpu_offer(IOService *,void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);
extern "C" void native_gop_presenter_set_source(IOService *presenter,IOService *nub);
volatile uint32_t native_legacy_gpu_fallbacks,native_legacy_gpu_last_status;
static IOService *activeLegacyDisplay;
// Keep one diagnostic mapping alive until reboot, even after backend stop.
// The pulse is permanently closed before any system-frame copy.
static IOMemoryMap *diagnosticAnchorMapping;
extern "C" int boot_progress_pulse_anchor(uintptr_t,uint64_t,unsigned,unsigned,unsigned) __attribute__((weak_import));
static IORegistryEntry *unitScaleChosen;
static bool unitScaleAdded,unitRotationAdded;
static void finishUnitScalePolicy(bool keep) {
    if(unitScaleChosen && !keep) {
        if(unitScaleAdded)unitScaleChosen->removeProperty("display-scale");
        if(unitRotationAdded)unitScaleChosen->removeProperty("display-rotation");
    }
    OSSafeReleaseNULL(unitScaleChosen);unitScaleAdded=false;unitRotationAdded=false;
}
static bool publishOrMatchWord(IORegistryEntry *entry,const char *key,uint32_t value,bool &added) {
    OSObject *previous=entry->getProperty(key);
    if(previous) {
        OSData *data=OSDynamicCast(OSData,previous);uint32_t word=0;
        if(!data || data->getLength()!=sizeof(word))return false;
        memcpy(&word,data->getBytesNoCopy(),sizeof(word));return word==value;
    }
    OSData *data=OSData::withBytes(&value,sizeof(value));
    bool result=data && entry->setProperty(key,data);OSSafeReleaseNULL(data);added=result;return result;
}
extern "C" {
volatile uint32_t native_legacy_stage;
volatile int native_legacy_meta_status=-1;
volatile uint32_t native_legacy_init_result,native_legacy_start_result,native_legacy_probe_match;
volatile uint32_t native_legacy_parent_match,native_legacy_object_size,native_legacy_released;
volatile uint64_t native_legacy_object,native_legacy_nub,native_legacy_workloop;
volatile uint32_t native_legacy_guard_slot;
volatile uint64_t native_legacy_guard_object,native_legacy_guard_caller;
volatile int native_legacy_surface_status=-1;
volatile uint32_t native_legacy_surface_found;
volatile uint64_t native_legacy_video_base,native_legacy_video_bytes;
volatile uint64_t native_legacy_gop_map,native_legacy_gop_virtual,native_legacy_gop_physical,native_legacy_gop_bytes;
volatile uint32_t native_legacy_gop_width,native_legacy_gop_height;
volatile uint32_t native_legacy_registration_calls,native_legacy_properties_length;
volatile uint32_t native_legacy_dt_attached;
volatile int native_legacy_unit_scale_status=-1;
volatile uint32_t native_legacy_unit_scale_calls,native_legacy_chosen_scale,native_legacy_chosen_rotation;
volatile uint32_t native_legacy_pm_stage,native_legacy_pm_calls,native_legacy_pm_callbacks,native_legacy_pm_ordinal;
volatile int native_legacy_pm_status=-1;
volatile uint32_t native_legacy_swap_validation_calls,native_legacy_swap_validation_result,native_legacy_swap_validation_reason;
volatile uint64_t native_legacy_swap_request,native_legacy_swap_surface;
volatile uint32_t native_legacy_transfer_calls;
volatile int native_legacy_transfer_status=-1;
volatile uint64_t native_legacy_transfer_bytes,native_legacy_transfer_request;
volatile uint32_t native_legacy_retirement_calls,native_legacy_retired_generation,native_legacy_presented_generation,native_legacy_retired_frames;
volatile uint64_t native_legacy_retired_current,native_legacy_retired_pending;
char native_legacy_properties[4096];
volatile uint32_t native_legacy_client_calls,native_legacy_client_type;
volatile int native_legacy_client_pid,native_legacy_client_status=-1;
volatile uint64_t native_legacy_client_task,native_legacy_client_object;
char native_legacy_client_class[64];
extern kern_return_t OSKextLoadKextWithIdentifier(const char *);
// patches/xnu/evb10-vc-mirror.patch: mirror of the console on the frame buffer (boot-arg vc_mirror).
extern bool evb10_vc_mirror;
extern int boot_progress_prepare_scanout(void) __attribute__((weak_import));
extern void boot_progress_cancel_scanout(void) __attribute__((weak_import));
extern void boot_progress_stage(const char *) __attribute__((weak_import));
volatile uint32_t native_legacy_prepare_calls,native_legacy_prepare_reason,native_legacy_prepare_attempts;
volatile uint32_t native_legacy_prepare_failures,native_legacy_prepare_retried,native_legacy_prepare_max_attempts;
volatile uint64_t native_legacy_prepare_wait_us;
static uint64_t probeNowUs(void) {uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000;}
bool native_legacy_prepare_scanout(void) {
    native_legacy_prepare_calls=native_legacy_prepare_calls+1;native_legacy_prepare_reason=0;native_legacy_prepare_attempts=0;
    if (!boot_progress_prepare_scanout) return true;
    if (ml_at_interrupt_context()) {native_legacy_prepare_reason=1;return false;}
    if (!ml_get_interrupts_enabled()) {native_legacy_prepare_reason=2;return false;}
    // Yield to any in-flight diagnostic writer; never spin on a preempted
    // holder or claim a frame that the console can still overwrite.
    const uint64_t began=probeNowUs();
    for (unsigned attempt=0;attempt<100;++attempt) {
        native_legacy_prepare_attempts=attempt+1;
        if (boot_progress_prepare_scanout()) {
            if(attempt) {
                native_legacy_prepare_retried=native_legacy_prepare_retried+1;native_legacy_prepare_wait_us=native_legacy_prepare_wait_us+(probeNowUs()-began);
                if(attempt+1>native_legacy_prepare_max_attempts)native_legacy_prepare_max_attempts=attempt+1;
            }
            return true;
        }
        IOSleep(1);
    }
    native_legacy_prepare_failures=native_legacy_prepare_failures+1;native_legacy_prepare_wait_us=native_legacy_prepare_wait_us+(probeNowUs()-began);
    native_legacy_prepare_reason=3;
    if(boot_progress_cancel_scanout)boot_progress_cancel_scanout();
    return false;
}
__attribute__((noinline)) void native_legacy_display_checkpoint(void) {__asm__ volatile("" ::: "memory");}
__attribute__((noinline)) void native_legacy_gop_mapping_checkpoint(void) {__asm__ volatile("" ::: "memory");}
__attribute__((noinline)) void native_legacy_client_checkpoint(void) {__asm__ volatile("" ::: "memory");}
void native_legacy_pm_observed(unsigned stage,IOReturn status,unsigned long ordinal) {
    native_legacy_pm_stage=stage;native_legacy_pm_status=status;native_legacy_pm_ordinal=(uint32_t)ordinal;
    native_legacy_pm_calls=native_legacy_pm_calls+1;
    if(stage==3)native_legacy_pm_callbacks=native_legacy_pm_callbacks+1;
}
__attribute__((noinline)) void native_legacy_swap_validation_checkpoint(bool,unsigned,const void *,const void *) {__asm__ volatile("" ::: "memory");}
// Last rejected swap request, copied from the original request layout the validator reads (offsets
// qualified in NativeLegacyDisplay::validateSwap). Observation only; nothing here changes the verdict.
volatile uint32_t native_legacy_swap_accepted_count,native_legacy_swap_rejected_count,native_legacy_rej_reason;
volatile uint64_t native_legacy_rej_surfaces[4];
volatile uint32_t native_legacy_rej_state[3],native_legacy_rej_options[3],native_legacy_rej_size[2],native_legacy_rej_rects[2][4];
char native_legacy_rej_class[32];
volatile uint64_t native_legacy_rej_surface[7];   // width, height, format, planes, stride, allocation, descriptor bytes of the last rejected surface
void native_legacy_surface_rejected(uint64_t width,uint64_t height,uint32_t format,uint32_t planes,uint64_t stride,uint64_t allocation,uint64_t bytes) {
    native_legacy_rej_surface[0]=width;native_legacy_rej_surface[1]=height;native_legacy_rej_surface[2]=format;native_legacy_rej_surface[3]=planes;
    native_legacy_rej_surface[4]=stride;native_legacy_rej_surface[5]=allocation;native_legacy_rej_surface[6]=bytes;
}
static void recordRejectedSwap(unsigned reason,const void *request,const void *surface) {
    const unsigned char *bytes=reinterpret_cast<const unsigned char *>(request);
    if(!bytes)return;
    native_legacy_rej_reason=reason;
    uint64_t surfaces[4]={};memcpy(surfaces,bytes+2624,sizeof(surfaces));
    for(unsigned i=0;i<4;++i)native_legacy_rej_surfaces[i]=surfaces[i];
    const unsigned stateOffsets[3]={792,796,800},optionOffsets[3]={804,808,812};
    for(unsigned i=0;i<3;++i){uint32_t a=0,b=0;memcpy(&a,bytes+stateOffsets[i],4);memcpy(&b,bytes+optionOffsets[i],4);
        native_legacy_rej_state[i]=a;native_legacy_rej_options[i]=b;}
    uint32_t w=0,h=0;memcpy(&w,bytes+816,4);memcpy(&h,bytes+820,4);native_legacy_rej_size[0]=w;native_legacy_rej_size[1]=h;
    const unsigned rectOffsets[2]={17,81};
    for(unsigned r=0;r<2;++r){uint32_t rect[4]={};memcpy(rect,bytes+rectOffsets[r],sizeof(rect));
        for(unsigned i=0;i<4;++i)native_legacy_rej_rects[r][i]=rect[i];}
    char name[32]={};
    if(surface){const OSObject *object=reinterpret_cast<const OSObject *>(surface);
        const OSMetaClass *meta=object->getMetaClass();const char *cls=meta?meta->getClassName():nullptr;
        if(cls)strlcpy(name,cls,sizeof(name));}
    memcpy(native_legacy_rej_class,name,sizeof(name));
    // The first few, in the log: touch3 (1080x1920 kernel frame buffer) rejected every swap with reason 3 (rectangles / size).
    static unsigned logged;
    if(logged<6) {
        ++logged;
        printf("evb10: display: swap rejected, reason %u, request size %ux%u (frame buffer %ux%u), src rect %u %u %u %u, dst rect %u %u %u %u\n",
            reason,w,h,evb10_fb_w,evb10_fb_h,native_legacy_rej_rects[0][0],native_legacy_rej_rects[0][1],native_legacy_rej_rects[0][2],
            native_legacy_rej_rects[0][3],native_legacy_rej_rects[1][0],native_legacy_rej_rects[1][1],native_legacy_rej_rects[1][2],
            native_legacy_rej_rects[1][3]);
        printf("evb10: display: swap rejected, state %x %x %x options %x %x %x, surface %s\n",native_legacy_rej_state[0],native_legacy_rej_state[1],
            native_legacy_rej_state[2],native_legacy_rej_options[0],native_legacy_rej_options[1],native_legacy_rej_options[2],name);
        printf("evb10: display: swap rejected, surface %llux%llu format %llx planes %llu stride %llu allocation %llu descriptor %llu bytes\n",
            (unsigned long long)native_legacy_rej_surface[0],(unsigned long long)native_legacy_rej_surface[1],(unsigned long long)native_legacy_rej_surface[2],
            (unsigned long long)native_legacy_rej_surface[3],(unsigned long long)native_legacy_rej_surface[4],(unsigned long long)native_legacy_rej_surface[5],
            (unsigned long long)native_legacy_rej_surface[6]);
    }
}
// Content statistics of presented frames (copy = kind 0, blank fill = kind 1): the first 32 are kept, with the time
// in seconds since the first frame. A per-frame record shows the animation cadence and when the OS blanks the panel.
#define NATIVE_LEGACY_FRAMES 32
volatile uint32_t native_legacy_frame_count;
volatile uint32_t native_legacy_frame_kind[NATIVE_LEGACY_FRAMES];
volatile uint32_t native_legacy_frame_time[NATIVE_LEGACY_FRAMES];
volatile uint64_t native_legacy_frame_stats[NATIVE_LEGACY_FRAMES][5];
static uint64_t frameEpoch;
void native_legacy_frame_observed(unsigned kind,uint64_t count,uint64_t minx,uint64_t miny,uint64_t maxx,uint64_t maxy) {
    uint32_t n=native_legacy_frame_count;
    uint64_t now,frequency;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(now));__asm__ volatile("mrs %0, cntfrq_el0":"=r"(frequency));
    if(!frameEpoch)frameEpoch=now;
    if(n<NATIVE_LEGACY_FRAMES) {
        native_legacy_frame_kind[n]=kind;
        native_legacy_frame_time[n]=frequency?(uint32_t)((now-frameEpoch)/frequency):0;
        native_legacy_frame_stats[n][0]=count;native_legacy_frame_stats[n][1]=minx;native_legacy_frame_stats[n][2]=miny;
        native_legacy_frame_stats[n][3]=maxx;native_legacy_frame_stats[n][4]=maxy;
    }
    native_legacy_frame_count=n+1;
}
volatile uint32_t native_legacy_frame_colors[8][4];
volatile uint64_t native_legacy_frame_color_counts[8][4],native_legacy_frame_color_other[8];
// Called right after the matching native_legacy_frame_observed(); fills slot count-1 (or the blank slot).
void native_legacy_frame_colors_observed(const uint32_t *colors,const uint64_t *counts,uint64_t other) {
    uint32_t n=native_legacy_frame_count;
    if(!n || n>8)return;   // dominant colours are kept for the first eight frames only
    n-=1;
    for(unsigned i=0;i<4;++i){native_legacy_frame_colors[n][i]=colors[i];native_legacy_frame_color_counts[n][i]=counts[i];}
    native_legacy_frame_color_other[n]=other;
}
// The life of every accepted swap, from the kernel's side: validated (the client submitted it), offered to the GPU copy (or started on the CPU),
// and retired. Times are microseconds since boot; the last 64 are kept.
struct SwapEvent {uint64_t submitted,offered,done;uint32_t number;};
static SwapEvent swapEvents[64];static uint32_t swapEventCount;
extern "C" void native_legacy_swap_stage(unsigned stage) {
    const uint64_t now=probeNowUs();
    uint32_t total=swapEventCount;
    if(stage==0) {
        SwapEvent &e=swapEvents[total%64];e={now,0,0,total+1};swapEventCount=total+1;return;
    }
    uint32_t first=total>64?total-64:0;
    for(uint32_t i=first;i<total;++i) {
        SwapEvent &e=swapEvents[i%64];
        if(stage==1 && !e.offered && !e.done){e.offered=now;return;}
        if(stage==2 && !e.done){e.done=now;if(!e.offered)e.offered=now;return;}
    }
}
extern "C" unsigned native_legacy_swap_timeline(char *out,unsigned cap) {
    unsigned at=0;out[0]=0;
    uint32_t total=swapEventCount,first=total>40?total-40:0;
    for(uint32_t i=first;i<total && at+90<cap;++i) {
        const SwapEvent &e=swapEvents[i%64];
        uint64_t previous=i>first?swapEvents[(i-1)%64].submitted:e.submitted;
        at+=snprintf(out+at,cap-at,"W%u t=%llu.%03llus +%llums offer=%lldms done=%lldms\n",e.number,(unsigned long long)(e.submitted/1000000),
            (unsigned long long)((e.submitted/1000)%1000),(unsigned long long)((e.submitted-previous)/1000),
            e.offered?(long long)((e.offered-e.submitted)/1000):-1LL,e.done?(long long)((e.done-e.submitted)/1000):-1LL);
    }
    return at;
}
// The display refresh tick (NativeLegacyDisplay, sheng-vsync): vsync messages sent, arms, idle stops, late ticks, starts, refresh
// properties reported at that rate, and the rate.
static volatile uint64_t vsyncEvents[6];
static volatile unsigned vsyncHz;
void native_legacy_vsync_observed(unsigned event,unsigned hz) {
    if(event<6)vsyncEvents[event]=vsyncEvents[event]+1;
    vsyncHz=hz;
}
extern "C" unsigned native_legacy_prepare_report(char *out,unsigned cap) {
    return snprintf(out,cap,"prepare calls=%u failures=%u retried=%u max_attempts=%u waited_ms=%llu vsync hz=%u sent=%llu arms=%llu idle=%llu late=%llu start=%llu refresh=%llu",
        native_legacy_prepare_calls,native_legacy_prepare_failures,native_legacy_prepare_retried,native_legacy_prepare_max_attempts,
        (unsigned long long)(native_legacy_prepare_wait_us/1000),vsyncHz,(unsigned long long)vsyncEvents[1],(unsigned long long)vsyncEvents[0],
        (unsigned long long)vsyncEvents[2],(unsigned long long)vsyncEvents[3],(unsigned long long)vsyncEvents[4],(unsigned long long)vsyncEvents[5]);
}
// When frames happened (cntvct ticks), for the frame rate: swaps the render server handed over, and frames that reached the scanout.
struct FrameClock {uint64_t t[1024];uint32_t n,reported;};
static FrameClock swapClock,shownClock;
static inline void frameClockNote(FrameClock &c) {
    uint64_t now;__asm__ volatile("mrs %0, cntvct_el0":"=r"(now));
    c.t[c.n%1024]=now;c.n=c.n+1;
}
// The frames since the last report: how many, the busiest second, and the shortest, mean and longest gap between two of them (microseconds).
static void frameClockReport(const char *name,FrameClock &c,uint32_t beat) {
    uint64_t frequency=0;__asm__ volatile("mrs %0, cntfrq_el0":"=r"(frequency));
    const uint32_t n=c.n;uint32_t count=n-c.reported;c.reported=n;
    if(!frequency || count<2)return;
    if(count>1024)count=1024;
    const uint32_t first=n-count;
    uint32_t best=0,lo=0;uint64_t shortest=UINT64_MAX,longest=0;
    for(uint32_t i=0;i<count;++i) {
        const uint64_t at=c.t[(first+i)%1024];
        while(at-c.t[(first+lo)%1024]>frequency)++lo;
        if(i-lo+1>best)best=i-lo+1;
        if(i) {
            const uint64_t gap=at-c.t[(first+i-1)%1024];
            if(gap<shortest)shortest=gap;if(gap>longest)longest=gap;
        }
    }
    const uint64_t span=c.t[(n-1)%1024]-c.t[first%1024];
    printf("evb10: display t+%us: %s: %u frames since the last report, busiest second %u, gap shortest %llu us, mean %llu us, longest %llu us\n",beat*5,name,count,best,
        (unsigned long long)(shortest*1000000/frequency),(unsigned long long)(span*1000000/frequency/(count-1)),(unsigned long long)(longest*1000000/frequency));
}
void native_legacy_swap_validation_observed(bool accepted,unsigned reason,const void *request,const void *surface) {
    if(accepted)native_legacy_swap_stage(0);
    if(accepted){native_legacy_swap_accepted_count=native_legacy_swap_accepted_count+1;frameClockNote(swapClock);}
    else {native_legacy_swap_rejected_count=native_legacy_swap_rejected_count+1;recordRejectedSwap(reason,request,surface);}
    native_legacy_swap_validation_calls=native_legacy_swap_validation_calls+1;
    native_legacy_swap_validation_result=accepted;native_legacy_swap_validation_reason=reason;
    native_legacy_swap_request=(uintptr_t)request;native_legacy_swap_surface=(uintptr_t)surface;
    native_legacy_swap_validation_checkpoint(accepted,reason,request,surface);
}
// Time of the pixel copy into the scanout buffer (counter ticks): swaps completed, last/maximum/sum, for the banner.
volatile uint64_t native_legacy_copy_n,native_legacy_copy_last,native_legacy_copy_max,native_legacy_copy_sum,native_legacy_copy_bytes_last,native_legacy_copy_bytes_sum;
void native_legacy_copy_timing_observed(uint64_t ticks,uint64_t bytes) {
    native_legacy_copy_bytes_last=bytes;native_legacy_copy_bytes_sum=native_legacy_copy_bytes_sum+bytes;
    native_legacy_copy_n=native_legacy_copy_n+1;native_legacy_copy_last=ticks;native_legacy_copy_sum=native_legacy_copy_sum+ticks;
    if(ticks>native_legacy_copy_max)native_legacy_copy_max=ticks;
}
__attribute__((noinline)) void native_legacy_transfer_observed(IOReturn status,const void *request,uint64_t bytes) {
    native_legacy_transfer_calls=native_legacy_transfer_calls+1;
    if(status==kIOReturnSuccess){native_legacy_swap_stage(2);frameClockNote(shownClock);}
    native_legacy_transfer_status=status;native_legacy_transfer_request=(uintptr_t)request;native_legacy_transfer_bytes=bytes;
    if(status!=kIOReturnSuccess && boot_progress_stage) {
        char message[120];snprintf(message,sizeof(message),"DISPLAY COPY FAILED status=%08x prepare=%u tries=%u",
          (unsigned)status,native_legacy_prepare_reason,native_legacy_prepare_attempts);
        boot_progress_stage(message);
    }
}
bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *memory,uint64_t stride,uint32_t width,uint32_t height) {
    return gpuHandoffPresenter && native_gop_gpu_offer(gpuHandoffPresenter,display,memory,stride,width,height);
}
void native_legacy_gpu_fallback_observed(int status) {
    native_legacy_gpu_last_status=(uint32_t)status;native_legacy_gpu_fallbacks=native_legacy_gpu_fallbacks+1;
}
__attribute__((noinline)) void native_legacy_retirement_observed(IOService *display) {
    const unsigned char *bytes=reinterpret_cast<const unsigned char *>(display);
    uint64_t current=0,pending=0;uint32_t completed=0,presented=0,frames=0;
    memcpy(&current,bytes+5472,8);memcpy(&pending,bytes+5480,8);
    memcpy(&completed,bytes+5820,4);memcpy(&presented,bytes+5824,4);memcpy(&frames,bytes+252,4);
    native_legacy_retired_current=current;native_legacy_retired_pending=pending;
    native_legacy_retired_generation=completed;native_legacy_presented_generation=presented;native_legacy_retired_frames=frames;
    native_legacy_retirement_calls=native_legacy_retirement_calls+1;
}
// The display object's properties as they are now (the registration copy above predates setDisplayRefreshProperties and the rest): for the
// UFS log. Returns the bytes written (NUL-terminated).
extern "C" unsigned native_legacy_properties_now(char *out,unsigned cap) {
    if(!out || !cap)return 0;
    out[0]=0;
    IOService *display=reinterpret_cast<IOService *>(native_legacy_object);
    if(!display)return 0;
    OSSerialize *xml=OSSerialize::withCapacity(8192);
    unsigned size=0;
    if(xml && display->serializeProperties(xml)) {
        size=xml->getLength();if(size>=cap)size=cap-1;
        memcpy(out,xml->text(),size);out[size]=0;
    }
    OSSafeReleaseNULL(xml);
    return size;
}
void native_legacy_registration_observed(IOService *display) {
    native_legacy_registration_calls=native_legacy_registration_calls+1;
    OSSerialize *xml=OSSerialize::withCapacity(2048);
    if(xml && display->serializeProperties(xml)) {
        unsigned size=xml->getLength();if(size>=sizeof(native_legacy_properties))size=sizeof(native_legacy_properties)-1;
        memcpy(native_legacy_properties,xml->text(),size);native_legacy_properties[size]=0;native_legacy_properties_length=size;
    }
    OSSafeReleaseNULL(xml);
}
void native_legacy_client_enter(task_t task,uint32_t type) {
    native_legacy_client_calls=native_legacy_client_calls+1;native_legacy_client_task=(uintptr_t)task;
    native_legacy_client_type=type;native_legacy_client_pid=task?proc_pid((proc_t)get_bsdtask_info(task)):0;
}
// Every framebuffer user client seen (object, pid), with what it does: the original client's vtable is copied and two entries (externalMethod, slot 168,
// and registerNotificationPort(port,type,refCon), slot 175) are pointed at wrappers that count and forward to the original function. The original code
// is not touched; only this object's vptr names the copy. XNU's ARM64 vptr points at slot 0 (the original code dispatches with ldr x8,[vptr,#slot*8]).
enum {kSeenClients=8,kSelectors=96,kRegEvents=8,kVtableSlots=256};
struct ClientStat {
    uintptr_t object;int pid;uint32_t calls,fails;uint32_t selector_calls[kSelectors],selector_fails[kSelectors],selector_status[kSelectors];
    unsigned regs;struct {uint32_t type;uint64_t port;uint32_t ref,status;} reg[kRegEvents];
    uint64_t sel6_first[3],sel6_last[3];uint32_t sel6_distinct;uint64_t sel6_seen[8][3];uint32_t sel6_calls[8],sel6_fails[8],sel6_status[8];
    uint32_t seq_count;struct {uint32_t a,b,c,status,done,presented;} seq[24];
    uintptr_t original_external,original_register;
};
static ClientStat seenClients[kSeenClients];static unsigned seenClientCount;
typedef IOReturn (*ExternalFn)(IOUserClient *,uint32_t,IOExternalMethodArguments *,IOExternalMethodDispatch *,OSObject *,void *);
typedef IOReturn (*RegisterFn)(IOUserClient *,mach_port_t,UInt32,UInt32);
static ClientStat *statFor(IOUserClient *client) {
    for(unsigned i=0;i<seenClientCount;++i)if(seenClients[i].object==(uintptr_t)client)return &seenClients[i];
    return nullptr;
}
static IOReturn wrappedExternal(IOUserClient *client,uint32_t selector,IOExternalMethodArguments *args,IOExternalMethodDispatch *dispatch,OSObject *target,void *ref) {
    ClientStat *stat=statFor(client);
    IOReturn status=stat?reinterpret_cast<ExternalFn>(stat->original_external)(client,selector,args,dispatch,target,ref):kIOReturnNotAttached;
    if(stat) {
        if(selector==6 && args && args->scalarInput && args->scalarInputCount>=3) {
            const uint64_t *in=args->scalarInput;
            if(!stat->sel6_distinct && !stat->sel6_first[0] && !stat->sel6_first[1])memcpy(stat->sel6_first,in,sizeof stat->sel6_first);
            memcpy(stat->sel6_last,in,sizeof stat->sel6_last);
            int at=-1;
            for(unsigned k=0;k<stat->sel6_distinct && k<8;++k)if(!memcmp(stat->sel6_seen[k],in,24))at=(int)k;
            if(at<0 && stat->sel6_distinct<8){at=(int)stat->sel6_distinct++;memcpy(stat->sel6_seen[at],in,24);}
            if(stat->seq_count<24){
                auto &e=stat->seq[stat->seq_count++];e.a=(uint32_t)in[0];e.b=(uint32_t)in[1];e.c=(uint32_t)in[2];e.status=(uint32_t)status;
                const unsigned char *object=reinterpret_cast<const unsigned char *>(native_legacy_object);
                e.done=e.presented=0;if(object){memcpy(&e.done,object+5820,4);memcpy(&e.presented,object+5824,4);}
            }
            if(at>=0){++stat->sel6_calls[at];if(status)++stat->sel6_fails[at];stat->sel6_status[at]=(uint32_t)status;}
        }
        ++stat->calls;if(status)++stat->fails;
        if(selector<kSelectors){++stat->selector_calls[selector];if(status)++stat->selector_fails[selector];stat->selector_status[selector]=(uint32_t)status;}
    }
    return status;
}
static IOReturn wrappedRegister(IOUserClient *client,mach_port_t port,UInt32 type,UInt32 refCon) {
    ClientStat *stat=statFor(client);
    IOReturn status=stat?reinterpret_cast<RegisterFn>(stat->original_register)(client,port,type,refCon):kIOReturnNotAttached;
    if(stat && stat->regs<kRegEvents){auto &e=stat->reg[stat->regs++];e.type=type;e.port=(uint64_t)(uintptr_t)port;e.ref=refCon;e.status=(uint32_t)status;}
    return status;
}
static void interpose(IOUserClient *client,int pid) {
    if(seenClientCount>=kSeenClients)return;
    ClientStat &stat=seenClients[seenClientCount];
    memset(&stat,0,sizeof stat);
    uintptr_t *vtable=*reinterpret_cast<uintptr_t **>(client);
    uintptr_t *copy=static_cast<uintptr_t *>(IOMalloc(kVtableSlots*sizeof(uintptr_t)));
    if(!copy)return;
    memcpy(copy,vtable,kVtableSlots*sizeof(uintptr_t));
    stat.object=(uintptr_t)client;stat.pid=pid;stat.original_external=copy[168];stat.original_register=copy[175];
    copy[168]=(uintptr_t)&wrappedExternal;copy[175]=(uintptr_t)&wrappedRegister;
    ++seenClientCount;
    *reinterpret_cast<uintptr_t **>(client)=copy;
}
// The display object's state words the original wait selector tests (offsets 5832..5864, as dwords), for the log.
extern "C" unsigned native_legacy_state_words(char *out,unsigned cap) {
    out[0]=0;
    const unsigned char *object=reinterpret_cast<const unsigned char *>(native_legacy_object);
    if(!object)return 0;
    unsigned at=snprintf(out,cap,"d+5808:");
    for(unsigned o=5808;o<5872 && at+16<cap;o+=4){uint32_t v;memcpy(&v,object+o,4);at+=snprintf(out+at,cap-at," %x",v);}
    return at;
}
extern "C" unsigned native_legacy_clients_dump(char *out,unsigned cap) {
    unsigned at=0;out[0]=0;
    for(unsigned c=0;c<seenClientCount && at+120<cap;++c) {
        const ClientStat &s=seenClients[c];
        at+=snprintf(out+at,cap-at,"C%u pid=%d calls=%u fails=%u regs=%u",c,s.pid,s.calls,s.fails,s.regs);
        for(unsigned r=0;r<s.regs && r<kRegEvents && at+60<cap;++r)
            at+=snprintf(out+at,cap-at," reg(t%u,p%llx,r%x)=%x",s.reg[r].type,(unsigned long long)s.reg[r].port,s.reg[r].ref,s.reg[r].status);
        at+=snprintf(out+at,cap-at,"\n");
        for(unsigned k=0;k<s.sel6_distinct && k<8 && at+100<cap;++k)
            at+=snprintf(out+at,cap-at,"  sel6 args %llx %llx %llx: %u calls %u fails last %x\n",(unsigned long long)s.sel6_seen[k][0],(unsigned long long)s.sel6_seen[k][1],(unsigned long long)s.sel6_seen[k][2],s.sel6_calls[k],s.sel6_fails[k],s.sel6_status[k]);
        for(unsigned k=0;k<s.seq_count && at+90<cap;++k)
            at+=snprintf(out+at,cap-at,"  s6#%u %x %x %x -> %x [5820]=%x [5824]=%x\n",k,s.seq[k].a,s.seq[k].b,s.seq[k].c,s.seq[k].status,s.seq[k].done,s.seq[k].presented);
        unsigned n=0;
        for(unsigned sel=0;sel<kSelectors && at+40<cap;++sel) {
            if(!s.selector_calls[sel])continue;
            if(!n)at+=snprintf(out+at,cap-at,"  sel");
            at+=snprintf(out+at,cap-at," %u:%u/%u/%x",sel,s.selector_calls[sel],s.selector_fails[sel],s.selector_status[sel]);
            if(++n%6==0)at+=snprintf(out+at,cap-at,"\n  sel");
        }
        if(n)at+=snprintf(out+at,cap-at,"\n");
    }
    return at;
}
void native_legacy_client_observed(IOReturn status,IOUserClient *client) {
    if(client && !status)interpose(client,native_legacy_client_pid);
    native_legacy_client_status=status;native_legacy_client_object=(uintptr_t)client;
    if(client)strlcpy(native_legacy_client_class,client->getMetaClass()->getClassName(),sizeof(native_legacy_client_class));
    native_legacy_client_checkpoint();
}
__attribute__((noinline)) bool native_legacy_gop_mapping_observed(IOService *object,IOMemoryMap *mapping,uint32_t width,uint32_t height) {
    if((uintptr_t)object!=native_legacy_object)panic("NativeLegacyDisplay: wrong mapping owner");
    native_legacy_gop_map=(uintptr_t)mapping;native_legacy_gop_virtual=mapping->getVirtualAddress();
    native_legacy_gop_physical=mapping->getPhysicalAddress();native_legacy_gop_bytes=mapping->getLength();
    native_legacy_gop_width=width;native_legacy_gop_height=height;
    if(boot_progress_pulse_anchor && !diagnosticAnchorMapping) {
        PE_Video anchorVideo={};
        if(!PE_current_console(&anchorVideo) && anchorVideo.v_depth==32 &&
           anchorVideo.v_rowBytes<=65536 &&
           anchorVideo.v_width==width && anchorVideo.v_height==height &&
           anchorVideo.v_baseAddr==mapping->getPhysicalAddress() && !anchorVideo.v_rotate) {
            mapping->retain();
            if(boot_progress_pulse_anchor(mapping->getVirtualAddress(),mapping->getLength(),width,height,
                (unsigned)anchorVideo.v_rowBytes))diagnosticAnchorMapping=mapping;
            else mapping->release();
        }
    }
    native_legacy_gop_mapping_checkpoint();
    unsigned enabled=0;PE_parse_boot_argn("offline-legacy-unit-scale",&enabled,sizeof(enabled));
    if(!enabled)return true;
    native_legacy_unit_scale_calls=native_legacy_unit_scale_calls+1;native_legacy_unit_scale_status=1;
    unsigned phase=0;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
    PE_Video video={};
    // The value is the UI scale: logical points = actual scanout pixels / scale (1..3). It changes
    // only the published display-scale; the scanout copy still moves one actual pixel per pixel.
    if(enabled<1 || enabled>3 || phase!=4 || unitScaleChosen || PE_current_console(&video) ||
       video.v_width!=width || video.v_height!=height || video.v_rotate ||
       mapping->getPhysicalAddress()!=video.v_baseAddr)return false;
    // Explicit offline UI policy: `enabled` actual GOP scanout pixels per logical
    // point in each direction (1 = native pixels). This is not physical pixel density,
    // panel timing, an Apple device identity or a synthesized mode list.
    unitScaleChosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
    if(!unitScaleChosen || !publishOrMatchWord(unitScaleChosen,"display-scale",enabled,unitScaleAdded) ||
       !publishOrMatchWord(unitScaleChosen,"display-rotation",video.v_rotate,unitRotationAdded)) {
        finishUnitScalePolicy(false);return false;
    }
    OSData *scale=OSDynamicCast(OSData,unitScaleChosen->getProperty("display-scale"));
    OSData *rotation=OSDynamicCast(OSData,unitScaleChosen->getProperty("display-rotation"));
    uint32_t actualScale=0,actualRotation=0;
    if(!scale || !rotation || scale->getLength()!=4 || rotation->getLength()!=4) {
        finishUnitScalePolicy(false);return false;
    }
    memcpy(&actualScale,scale->getBytesNoCopy(),4);memcpy(&actualRotation,rotation->getBytesNoCopy(),4);
    native_legacy_chosen_scale=actualScale;native_legacy_chosen_rotation=actualRotation;
    native_legacy_unit_scale_status=actualScale==enabled && actualRotation==video.v_rotate?0:2;
    if(native_legacy_unit_scale_status){finishUnitScalePolicy(false);return false;}
    return true;
}
__attribute__((noreturn,noinline)) void native_legacy_backend_guard(unsigned slot,void *object,void *caller) {
    native_legacy_guard_slot=slot;native_legacy_guard_object=(uintptr_t)object;native_legacy_guard_caller=(uintptr_t)caller;
    panic("NativeLegacyDisplay: leaf=%u caller=%llx stage=%u PM=%u swap=%u/%u copy=%08x bytes=%llu retire=%u handoff=%u/%u",
      slot,(uint64_t)(uintptr_t)caller,native_legacy_stage,native_legacy_pm_stage,
      native_legacy_swap_validation_result,native_legacy_swap_validation_reason,
      (unsigned)native_legacy_transfer_status,native_legacy_transfer_bytes,native_legacy_retirement_calls,
      native_legacy_prepare_reason,native_legacy_prepare_attempts);
}
// One line per stage on the console: the globals above are otherwise only visible to a debugger, and this board has none.
// Kept short: a single kernel printf over ~255 characters loses its tail on this console.
static void evb10_display_report(const char *where) {
    printf("evb10: display %s: stage %u meta %d init %u size %#x parent %u probe %u start %u dt %u\n",where,native_legacy_stage,
        native_legacy_meta_status,native_legacy_init_result,native_legacy_object_size,native_legacy_parent_match,
        native_legacy_probe_match,native_legacy_start_result,native_legacy_dt_attached);
    printf("evb10: display %s: IOSurface kext %d found %u video %#llx+%#llx gop map %#llx %ux%u\n",where,native_legacy_surface_status,
        native_legacy_surface_found,(unsigned long long)native_legacy_video_base,(unsigned long long)native_legacy_video_bytes,
        (unsigned long long)native_legacy_gop_map,native_legacy_gop_width,native_legacy_gop_height);
}
// What the original userland does with the display, once it is live: this board has no debugger, and "the panel still shows the
// boot log" has two very different causes (nobody asks for a frame, or frames are asked for and refused). Every 5 s a thread call
// prints the client / swap / copy counters when they moved, and a heartbeat line every 30 s. Lines stay under ~200 characters.
// evb10-fb-wc (default 1): the frame buffer as write-combined normal memory instead of device memory. The display driver maps the nub's
// range with kIOMapInhibitCache, which is Device-nGnRnE on arm64 (every 8 byte store of the CPU copy waits for its own completion:
// sheng measured 143-285 ms per swap, so the UI could not exceed ~5 frames a second and the scan caught every copy half done = tearing).
// Normal non-cacheable memory (what VM_WIMG_WCOMB is) merges the stores into bursts and the copy's closing dsb still makes it visible
// to the display engine. pmap_set_cache_attributes changes the page for every mapping of it at once (the driver's, our kernel one and
// the physical aperture), so there is no mismatched alias. Called once the display is live (the driver has mapped the pages by then).
extern "C" void pmap_set_cache_attributes(unsigned int pn,unsigned int attributes);
static IOBufferMemoryDescriptor *evb10_fb_memory;   // the surface of evb10_fb_override
static bool evb10_fb_wc;
static void evb10_fb_writecombine(void) {
    unsigned want=1;PE_parse_boot_argn("evb10-fb-wc",&want,sizeof want);
    if(!want || !evb10_fb_memory || !evb10_fb_base || evb10_fb_wc)return;
    const uint64_t first=evb10_fb_base>>PAGE_SHIFT,pages=round_page((uint64_t)evb10_fb_row*evb10_fb_h)>>PAGE_SHIFT;
    for(uint64_t i=0;i<pages;++i)pmap_set_cache_attributes((unsigned int)(first+i),0x6U);   // VM_WIMG_WCOMB
    __asm__ volatile("dsb sy\n\tisb" ::: "memory");
    evb10_fb_wc=true;
    printf("evb10: fb: %llu pages of the frame buffer are write-combined now\n",(unsigned long long)pages);
}
extern "C" volatile uint32_t native_legacy_direct[16];
extern "C" volatile uint32_t evb10_vop_direct_stats[8];
// evb10-vop-fence-check=1: [0] flips checked, [1] words sampled, [2] words that changed when the GPU's caches were cleaned, [3]/[4] one example.
extern "C" volatile uint32_t evb10_vop_check[8];
extern "C" volatile uint64_t evb10_vop_check_at;
static thread_call_t displayReportCall;
static uint32_t displayReportBeat,displayReportSignature;
static void evb10_display_arm(void) {
    uint64_t deadline;clock_interval_to_deadline(5000,NSEC_PER_MSEC,&deadline);
    thread_call_enter_delayed(displayReportCall,deadline);
}
static void evb10_display_tick(thread_call_param_t,thread_call_param_t) {
    static char dump[1536];
    const uint32_t beat=++displayReportBeat;
    uint32_t calls=0;
    for(unsigned c=0;c<seenClientCount;++c)calls+=seenClients[c].calls;
    const uint32_t signature=calls*3+seenClientCount*5+native_legacy_client_calls*7+native_legacy_swap_accepted_count*11
        +native_legacy_swap_rejected_count*13+native_legacy_transfer_calls*17+native_legacy_retirement_calls*19
        +native_legacy_frame_count*23+(uint32_t)native_legacy_copy_n*29+(uint32_t)native_gop_gpu_offers*31+(uint32_t)native_gop_gpu_completions*37+swapClock.n*41+shownClock.n*43;
    const bool moved=signature!=displayReportSignature;
    if(moved || beat%6==0) {
        printf("evb10: display t+%us: opened %u (last pid %d type %u status %x %s) seen %u clients, %u calls\n",beat*5,native_legacy_client_calls,
            native_legacy_client_pid,native_legacy_client_type,(unsigned)native_legacy_client_status,native_legacy_client_class,seenClientCount,calls);
        printf("evb10: display t+%us: swap ok %u rej %u (reason %u) copy %u (status %x, %llu B, %llu copies) retired %u gen %u/%u frames %u\n",beat*5,
            native_legacy_swap_accepted_count,native_legacy_swap_rejected_count,native_legacy_rej_reason,native_legacy_transfer_calls,
            (unsigned)native_legacy_transfer_status,(unsigned long long)native_legacy_transfer_bytes,(unsigned long long)native_legacy_copy_n,
            native_legacy_retirement_calls,native_legacy_retired_generation,native_legacy_presented_generation,native_legacy_frame_count);
        frameClockReport("swaps handed over (the render server's rate)",swapClock,beat);
        frameClockReport("frames shown (the scanout's rate)",shownClock,beat);
        if(native_gop_gpu_offers || native_gop_wait_stats[0])
            printf("evb10: display t+%us: gpu hand-off: offered %llu completed %llu failed %llu abandoned %llu, early retires %llu (failed %llu), cpu fallbacks %u (last status %x), waits %llu (%llu timeouts)\n",
                beat*5,(unsigned long long)native_gop_gpu_offers,(unsigned long long)native_gop_gpu_completions,(unsigned long long)native_gop_gpu_failures,
                (unsigned long long)native_gop_gpu_abandoned,(unsigned long long)native_legacy_early_retires,(unsigned long long)native_legacy_early_failures,
                native_legacy_gpu_fallbacks,native_legacy_gpu_last_status,(unsigned long long)native_gop_wait_stats[0],(unsigned long long)native_gop_wait_stats[1]);
        if(native_legacy_direct[0] || native_legacy_direct[7] || evb10_vop_direct_stats[0])
            printf("evb10: display t+%us: vop direct (iommu %u, status %08x %08x): flips %u, surfaces mapped %u (map failures %u, let go %u, remaps %u, zap timeouts %u), latch mean %u us longest %u us (%u over 50 ms), copy fallbacks %u, back to boot %u\n",
                beat*5,evb10_vop_direct_stats[0],evb10_vop_direct_stats[1],evb10_vop_direct_stats[2],native_legacy_direct[0],native_legacy_direct[1],
                native_legacy_direct[2],native_legacy_direct[6],evb10_vop_direct_stats[3],evb10_vop_direct_stats[4],
                native_legacy_direct[0]?native_legacy_direct[5]/native_legacy_direct[0]:0,native_legacy_direct[4],native_legacy_direct[3],
                native_legacy_direct[7],native_legacy_direct[8]);
        if(native_legacy_direct[9] || native_legacy_direct[13] || native_legacy_direct[12] || evb10_vop_check[0])
            printf("evb10: display t+%us: fence: %u of %u swaps arrived with the render server's GPU work not done; %u of %u flips waited for the GPU (mean %u us, longest %u us, %u gave up after 250 ms); L2 check: %u flips, %u samples, %u differ after a cache clean (examples %08x/%08x at %llu)\n",
                beat*5,native_legacy_direct[14],native_legacy_direct[13],native_legacy_direct[9],native_legacy_direct[0],native_legacy_direct[9]?native_legacy_direct[10]/native_legacy_direct[9]:0,
                native_legacy_direct[11],native_legacy_direct[12],evb10_vop_check[0],evb10_vop_check[1],evb10_vop_check[2],evb10_vop_check[3],evb10_vop_check[4],
                (unsigned long long)evb10_vop_check_at);
        // The pixel copy into the scanout buffer, per swap (the counter runs at cntfrq).
        uint64_t frequency=0;__asm__ volatile("mrs %0, cntfrq_el0":"=r"(frequency));
        if(frequency && native_legacy_copy_n)
            printf("evb10: display t+%us: copy of %llu B: average %llu us, longest %llu us, last %llu us over %llu copies (frame buffer %s)\n",beat*5,
                (unsigned long long)native_legacy_copy_bytes_last,(unsigned long long)(native_legacy_copy_sum*1000000/frequency/native_legacy_copy_n),
                (unsigned long long)(native_legacy_copy_max*1000000/frequency),(unsigned long long)(native_legacy_copy_last*1000000/frequency),
                (unsigned long long)native_legacy_copy_n,evb10_fb_wc?"write-combined":"uncached device memory");
    }
    if(moved) {
        char prepare[256];
        native_legacy_prepare_report(prepare,sizeof prepare);
        printf("evb10: display t+%us: %s\n",beat*5,prepare);
        const uint32_t frames=native_legacy_frame_count<NATIVE_LEGACY_FRAMES?native_legacy_frame_count:NATIVE_LEGACY_FRAMES;
        if(frames) {
            const uint32_t f=frames-1;
            printf("evb10: display t+%us: last frame #%u kind %u at %us: %llu pixels in (%llu,%llu)-(%llu,%llu)\n",beat*5,f,native_legacy_frame_kind[f],
                native_legacy_frame_time[f],(unsigned long long)native_legacy_frame_stats[f][0],(unsigned long long)native_legacy_frame_stats[f][1],
                (unsigned long long)native_legacy_frame_stats[f][2],(unsigned long long)native_legacy_frame_stats[f][3],(unsigned long long)native_legacy_frame_stats[f][4]);
        }
        native_legacy_clients_dump(dump,sizeof dump);
        unsigned lines=0;
        for(char *at=dump;*at && lines<14;) {
            char *end=at;while(*end && *end!='\n')++end;
            const bool last=!*end;*end=0;
            if(*at)printf("evb10: display c: %.200s\n",at);
            ++lines;at=last?end:end+1;
        }
    }
    displayReportSignature=signature;
    evb10_display_arm();
}
static void evb10_display_reporter_start(void) {
    if(displayReportCall)return;
    displayReportCall=thread_call_allocate(evb10_display_tick,nullptr);
    if(displayReportCall)evb10_display_arm();
}
// evb10-fb-w=<px> evb10-fb-h=<px> (2026-10-28): a frame buffer of the kernel's own instead of the firmware's. The UEFI GOP surface is
// 640x480 (what Vop2Dxe drives on HDMI); the panel is 1080x1920 portrait, and a 640x480 window in the middle of it is too small to
// read. evb10_vop_show_gop (patches/tools) puts whatever PE_state.video describes on the panel at the top left, so
// all this has to do is describe a bigger surface BEFORE the display driver, the nub of the late probe and the VOP read it.
// The surface is physically contiguous below 4 GiB (the VOP's window addresses are 32 bit) and uncached (the CPU copy of the UI
// writes straight to memory, nothing may sit dirty in a cache line the display engine cannot see). The console mirror and the
// kernel's own frame buffer mapping (video_console.c) keep the old surface: after "display live" nothing draws there any more.
static void evb10_fb_override(void) {
    unsigned w=0,h=0;
    if(!PE_parse_boot_argn("evb10-fb-w",&w,sizeof w) || !PE_parse_boot_argn("evb10-fb-h",&h,sizeof h) || evb10_fb_memory)return;
    PE_Video old={};
    if(PE_current_console(&old) || old.v_depth!=32 || old.v_rotate) {
        printf("evb10: fb: the firmware frame buffer is not 32 bit, left alone\n");return;
    }
    if(w<64 || h<64 || w>1080 || h>1920 || (w&1)) {
        printf("evb10: fb: %ux%u is outside what the panel takes (64..1080 x 64..1920, even width), left alone\n",w,h);return;
    }
    const uint64_t rowBytes=(uint64_t)w*4,bytes=round_page(rowBytes*h);
    IOBufferMemoryDescriptor *memory=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone)|kIOMemoryPhysicallyContiguous|kIOMapInhibitCache,
        bytes,0x00000000fffff000ULL);
    if(!memory){printf("evb10: fb: no %llu contiguous bytes below 4 GiB, left alone\n",bytes);return;}
    if(memory->prepare()!=kIOReturnSuccess){printf("evb10: fb: the frame buffer could not be wired, left alone\n");memory->release();return;}
    IOByteCount length=0;
    const uint64_t phys=memory->getPhysicalSegment(0,&length,kIOMemoryMapperNone);
    uint32_t *pixels=static_cast<uint32_t *>(memory->getBytesNoCopy());
    if(!pixels || !phys || length<bytes || phys+bytes>0x100000000ULL || (phys&0xfff)) {
        printf("evb10: fb: surface at %#llx, %llu contiguous bytes of %llu: not usable, left alone\n",phys,(uint64_t)length,bytes);
        (void)memory->complete();memory->release();return;
    }
    for(uint64_t i=0;i<bytes/4;++i)pixels[i]=0xff000000U;
    __asm__ volatile("dsb sy" ::: "memory");
    evb10_fb_memory=memory;
    evb10_fb_row=(uint32_t)rowBytes;evb10_fb_w=w;evb10_fb_h=h;evb10_fb_kva=(uintptr_t)pixels;
    __asm__ volatile("dmb ish" ::: "memory");
    evb10_fb_base=phys;
    printf("evb10: fb: %ux%u at %#llx (%llu bytes, kernel %p), was %lux%lu at %#lx\n",w,h,phys,bytes,static_cast<void *>(pixels),
        old.v_width,old.v_height,old.v_baseAddr);
}
// ---- Direct scanout (evb10-vop-direct=1): the VOP2 window fetches the compositor's surface itself -------------------------------------
// The CPU copy of a frame into the write-combined scanout buffer takes about 40 ms (8 MB), the GPU copy about 70 ms: neither can reach 60 fps.
// Nothing needs to be copied if the window can read the surface where the compositor left it. The surface is scattered 4 KiB pages anywhere
// in RAM (some above 4 GiB) and the window's address register is 32 bit, so the VOP's IOMMU (rockchip,iommu-v2, two banks at 0xfdd97e00 and
// 0xfdd97f00 behind the one switch of VOP_AXI) has to present it as one linear range. Its page table is 4 KiB pages in two levels (1024 x 1024
// entries; Linux drivers/iommu/rockchip-iommu.c, rk_mk_dte_v2 and rk_mk_pte_v2: the address in bits 31:12, bits 35:32 of it in bits 11:8, bits
// 39:36 in bits 7:4, flags in bits 3:0 with bit 0 valid, bit 1 readable).
//   RAM [0, 0xf0000000) is mapped to itself, so turning paging on changes nothing for what already scans out (the boot frame buffer, any other
//   window the firmware left on). 0xf0000000..0xffffffff are slots of 16 MiB for the compositor's surfaces: evb10_vop_direct_map fills one.
// The tables live in write-combined memory below 4 GiB; the IOTLB is shot down (stall, ZAP_CACHE, un-stall) after every remap.
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr,vm_size_t size);
extern "C" volatile uint32_t native_legacy_direct[16];
extern "C" volatile uint32_t evb10_vop_direct_stats[8];   // [0] initialised (1/2 failed), [1] STATUS of bank 0, [2] STATUS of bank 1, [3] remaps, [4] zap waits that timed out
namespace {
constexpr uint64_t kVopPhys=0xfdd90000ULL;
constexpr uint32_t kVopSize=0x10000,kMmuBank[2]={0x7e00,0x7f00};
constexpr uint32_t kIovaSlotBase=0xf0000000U,kIovaSlotBytes=0x01000000U,kIovaSlots=16,kIdentityTables=960;
constexpr uint32_t kVopWinMst=0x1e14,kVopWinVir=0x1e1c,kVopCfgDone=0,kVopCommit=0x00088008U;
volatile uint8_t *vopRegs;
IOBufferMemoryDescriptor *vopTables;
uint32_t *vopDt,*vopPt;          // the directory page, then 1024 table pages, as the kernel sees them
uint64_t vopTablesPhys;
bool vopDirectReady,vopDirectTried;
uint32_t vopFlipIova,vopFlipStride,vopShownIova,vopShownStride;   // flipped last / known to be on the panel (the screenshots read that)
uint64_t vopSlotPages[kIovaSlots][kIovaSlotBytes/4096];           // the physical pages behind each slot
uint32_t vopSlotCount[kIovaSlots];
inline uint32_t vopRead(uint32_t off){return *reinterpret_cast<volatile uint32_t *>(vopRegs+off);}
inline void vopWrite(uint32_t off,uint32_t value){*reinterpret_cast<volatile uint32_t *>(vopRegs+off)=value;}
inline uint32_t mmuRead(unsigned bank,uint32_t off){return vopRead(kMmuBank[bank]+off);}
inline void mmuWrite(unsigned bank,uint32_t off,uint32_t value){vopWrite(kMmuBank[bank]+off,value);}
void mmuCommand(uint32_t command){for(unsigned b=0;b<2;++b)mmuWrite(b,0x08,command);}
// Both banks' STATUS & mask == value, within 100 ms.
bool mmuWait(uint32_t mask,uint32_t value) {
    for(unsigned i=0;i<10000;++i) {
        if((mmuRead(0,0x04)&mask)==value && (mmuRead(1,0x04)&mask)==value)return true;
        IODelay(10);
    }
    return false;
}
// Linux rk_mk_dte_v2 / rk_mk_pte_v2: the same encoding of an address for a directory entry and a page entry.
inline uint32_t mmuEntry(uint64_t pa,uint32_t flags) {
    uint64_t v=(pa&0xfffff000ULL)|((pa&0xf00000000ULL)>>24)|((pa&0xf000000000ULL)>>32);
    return uint32_t(v&0xfffffff0ULL)|flags;
}
inline uint64_t tablePhys(uint32_t index){return vopTablesPhys+4096ULL*(1+index);}
}
volatile uint32_t evb10_vop_direct_stats[8];
extern "C" int (*evb10_direct_pixel_hook)(uint64_t byteOffset,uint32_t *value);
static int evb10_vop_direct_pixel(uint64_t byteOffset,uint32_t *value);
extern "C" bool evb10_vop_direct_init(void) {
    if(vopDirectReady)return true;
    if(vopDirectTried)return false;
    vopDirectTried=true;
    vopRegs=reinterpret_cast<volatile uint8_t *>(ml_io_map(kVopPhys,kVopSize));
    if(!vopRegs){evb10_vop_direct_stats[0]=2;printf("evb10: vop-direct: no mapping of the VOP\n");return false;}
    const uint32_t before[2]={mmuRead(0,0x04),mmuRead(1,0x04)};
    const uint64_t bytes=4096ULL*(1+1024);
    vopTables=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone)|kIOMemoryPhysicallyContiguous|kIOMapInhibitCache,
        bytes,0x00000000fffff000ULL);
    if(!vopTables){evb10_vop_direct_stats[0]=2;printf("evb10: vop-direct: no %llu contiguous bytes below 4 GiB for the page tables\n",bytes);return false;}
    IOByteCount length=0;
    if(vopTables->prepare()!=kIOReturnSuccess){evb10_vop_direct_stats[0]=2;printf("evb10: vop-direct: the page tables could not be wired\n");return false;}
    vopTablesPhys=vopTables->getPhysicalSegment(0,&length,kIOMemoryMapperNone);
    vopDt=static_cast<uint32_t *>(vopTables->getBytesNoCopy());
    if(!vopDt || !vopTablesPhys || length<bytes || (vopTablesPhys&0xfff) || vopTablesPhys+bytes>0x100000000ULL) {
        evb10_vop_direct_stats[0]=2;
        printf("evb10: vop-direct: tables at %#llx, %llu contiguous bytes of %llu: not usable\n",vopTablesPhys,(uint64_t)length,bytes);return false;
    }
    vopPt=vopDt+1024;
    // Write-combined pages: the 1 M stores of the identity map are bursts instead of device-memory round trips (the walker reads after the dsb).
    for(uint64_t i=0;i<bytes>>PAGE_SHIFT;++i)pmap_set_cache_attributes((unsigned int)((vopTablesPhys>>PAGE_SHIFT)+i),0x6U);
    for(uint32_t d=0;d<1024;++d) {
        vopDt[d]=mmuEntry(tablePhys(d),1);
        uint32_t *pt=vopPt+1024ULL*d;
        for(uint32_t i=0;i<1024;++i)
            pt[i]=d<kIdentityTables?mmuEntry((uint64_t(d)<<22)|(uint64_t(i)<<12),3):0;
    }
    __asm__ volatile("dsb sy\n\tisb" ::: "memory");
    // Linux rk_iommu_enable: stall, force reset, directory address, zap, interrupts off (polled), no DTE fetch time limit, paging on, un-stall.
    // (A stall can only be requested while paging is on: with it off the status stays IDLE and the command never takes: 0x18 on the first try.)
    if((before[0]&1) && (before[1]&1)) {
        mmuCommand(2);
        if(!mmuWait(0x4,0x4)){printf("evb10: vop-direct: stall timed out (status %08x %08x)\n",mmuRead(0,0x04),mmuRead(1,0x04));evb10_vop_direct_stats[0]=2;return false;}
    }
    mmuCommand(6);
    IODelay(100);
    for(unsigned b=0;b<2;++b) {
        mmuWrite(b,0x00,mmuEntry(vopTablesPhys,1));
        mmuWrite(b,0x08,4);
        mmuWrite(b,0x1c,0);
        mmuWrite(b,0x24,mmuRead(b,0x24)|0x80000000U);
    }
    mmuCommand(0);
    const bool paging=mmuWait(0x1,0x1);
    for(unsigned b=0;b<2;++b)if(mmuRead(b,0x04)&0x4)mmuWrite(b,0x08,3);
    mmuWait(0x4,0);
    evb10_vop_direct_stats[1]=mmuRead(0,0x04);evb10_vop_direct_stats[2]=mmuRead(1,0x04);
    printf("evb10: vop-direct: IOMMU status %08x %08x before, %08x %08x after (paging %s), tables at %#llx, dte %08x %08x, window %08x vir %08x\n",
        before[0],before[1],mmuRead(0,0x04),mmuRead(1,0x04),paging?"on":"NOT ON",vopTablesPhys,mmuRead(0,0x00),mmuRead(1,0x00),
        vopRead(kVopWinMst),vopRead(kVopWinVir));
    if(!paging){evb10_vop_direct_stats[0]=2;return false;}
    vopDirectReady=true;evb10_vop_direct_stats[0]=1;
    evb10_direct_pixel_hook=&evb10_vop_direct_pixel;
    return true;
}
// The boot frame buffer is mapped to its own address, so flipping back to it is a flip to its physical address.
extern "C" void evb10_vop_direct_boot(uint32_t *iova,uint32_t *stride) {*iova=uint32_t(evb10_fb_base);*stride=evb10_fb_row;}
// Maps a surface's pages into slot k and returns its address in the IOMMU's space (0 if it does not fit).
extern "C" uint32_t evb10_vop_direct_map(unsigned slot,const uint64_t *pages,unsigned count) {
    if(!vopDirectReady || slot>=kIovaSlots || !count || count>kIovaSlotBytes/4096)return 0;
    const uint32_t iova=kIovaSlotBase+slot*kIovaSlotBytes;
    uint32_t *pt=vopPt+1024ULL*(iova>>22);   // the slot's four tables, one after another
    for(unsigned i=0;i<kIovaSlotBytes/4096;++i)pt[i]=i<count?mmuEntry(pages[i],3):0;
    for(unsigned i=0;i<count;++i)vopSlotPages[slot][i]=pages[i];
    vopSlotCount[slot]=count;
    __asm__ volatile("dsb sy" ::: "memory");
    // A slot is remapped only while it is not scanned out, but the IOTLB may still hold the old surface's entries.
    mmuCommand(2);
    if(!mmuWait(0x4,0x4))evb10_vop_direct_stats[4]=evb10_vop_direct_stats[4]+1;
    mmuCommand(4);
    mmuCommand(3);
    evb10_vop_direct_stats[3]=evb10_vop_direct_stats[3]+1;
    return iova;
}
// A new frame: the window's address and stride, then the commit that latches them at the next frame start (the port bit of the commit
// does not read back as cleared; the window's own address register reads the latched value).
extern "C" void evb10_vop_direct_flip(uint32_t iova,uint32_t stride) {
    vopWrite(kVopWinMst,iova);
    vopWrite(kVopWinVir,stride/4);
    __asm__ volatile("dsb sy" ::: "memory");
    vopFlipIova=iova;vopFlipStride=stride;
    if(iova<kIovaSlotBase)vopShownIova=iova;   // the boot frame buffer: the screenshots read the frame buffer itself
    vopWrite(kVopCfgDone,kVopCommit);
    __asm__ volatile("dsb sy" ::: "memory");
}
extern "C" bool evb10_vop_direct_latched(void) {
    const bool latched=vopRead(kVopWinMst)==vopFlipIova;
    if(latched){vopShownStride=vopFlipStride;vopShownIova=vopFlipIova;}
    return latched;
}
// The screenshots (kernel patch evb10-userland-mounts, evb10_px_at) ask for a byte offset of the frame in the frame buffer's layout.
extern "C" unsigned int ml_phys_read_word(vm_offset_t paddr);
static int evb10_vop_direct_pixel(uint64_t byteOffset,uint32_t *value) {
    const uint32_t iova=vopShownIova,rowBytes=evb10_fb_row;
    if(iova<kIovaSlotBase || !rowBytes || !vopShownStride)return 0;
    const uint32_t slot=(iova-kIovaSlotBase)/kIovaSlotBytes;
    if(slot>=kIovaSlots)return 0;
    const uint64_t at=uint64_t(iova-kIovaSlotBase-slot*kIovaSlotBytes)+(byteOffset/rowBytes)*vopShownStride+byteOffset%rowBytes;
    if((at>>12)>=vopSlotCount[slot])return 0;
    *value=ml_phys_read_word(vm_offset_t(vopSlotPages[slot][at>>12]+(at&0xfff)));
    return 1;
}
// The check behind evb10-vop-fence-check=1: before a flip, sample words of the surface that is about to be shown, make the GPU clean its caches
// into memory (the host's GPU_CMD), and read the same words again. Words that changed were still sitting in the GPU (or still being drawn).
extern "C" bool mali_clean_caches(void);
extern "C" void flush_dcache64(unsigned long long addr,unsigned count,int phys);
volatile uint32_t evb10_vop_check[8];
volatile uint64_t evb10_vop_check_at;
extern "C" void evb10_vop_direct_check(uint32_t iova) {
    if(!vopDirectReady || iova<kIovaSlotBase)return;
    const uint32_t slot=(iova-kIovaSlotBase)/kIovaSlotBytes;
    if(slot>=kIovaSlots || !vopSlotCount[slot])return;
    constexpr unsigned kSamples=512;
    static uint32_t before[kSamples];
    static uint64_t where[kSamples];
    const uint64_t bytes=uint64_t(vopSlotCount[slot])*4096;
    const uint64_t step=(bytes/kSamples)&~uint64_t(3);
    const uint64_t shift=(uint64_t(evb10_vop_check[0])*52)&0x3f;   // another set of words each time
    for(unsigned i=0;i<kSamples;++i) {
        const uint64_t at=((uint64_t(i)*step+shift)%bytes)&~uint64_t(3);
        const uint64_t pa=vopSlotPages[slot][at>>12]+(at&0xfff);
        where[i]=pa;
        flush_dcache64(pa&~63ULL,64,1);
        before[i]=ml_phys_read_word(vm_offset_t(pa));
    }
    const bool cleaned=mali_clean_caches();
    uint32_t changed=0;
    for(unsigned i=0;i<kSamples;++i) {
        flush_dcache64(where[i]&~63ULL,64,1);
        const uint32_t after=ml_phys_read_word(vm_offset_t(where[i]));
        if(after!=before[i]) {
            if(!changed && !evb10_vop_check[2]){evb10_vop_check[3]=before[i];evb10_vop_check[4]=after;evb10_vop_check_at=where[i];}
            ++changed;
        }
    }
    evb10_vop_check[0]=evb10_vop_check[0]+1;evb10_vop_check[1]=evb10_vop_check[1]+kSamples;evb10_vop_check[2]=evb10_vop_check[2]+changed;
    if(!cleaned)evb10_vop_check[5]=evb10_vop_check[5]+1;
}
static void legacy_probe_body(void);
void native_legacy_display_probe(void);
void native_legacy_display_probe(void) {
    legacy_probe_body();
    evb10_display_report("probe");
}
static void legacy_probe_body(void) {
    // The registration path and optional original-userclient inspection both
    // reach this boot-thread entry. Run class constructors only once; a second
    // postModLoad would attempt to register the same metaclasses again.
    if(native_legacy_stage)return;
    native_legacy_stage=1;void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle){native_legacy_display_checkpoint();return;}
    native_legacy_display_static_init();native_gop_presenter_static_init();native_legacy_meta_status=OSMetaClass::postModLoad(handle);
    if(native_legacy_meta_status){native_legacy_display_checkpoint();return;}
    PE_Video video={};native_legacy_stage=2;
    evb10_fb_override();
    if(PE_current_console(&video) || !video.v_baseAddr || !video.v_width || !video.v_height || video.v_depth!=32
        || video.v_rotate || video.v_width>16384 || video.v_height>16384 || video.v_rowBytes<video.v_width*4
        || video.v_rowBytes>UINT64_MAX/video.v_height){native_legacy_display_checkpoint();return;}
    IOService *display=native_legacy_display_allocate();native_legacy_object=(uintptr_t)display;
    if(!display){native_legacy_display_checkpoint();return;}
    native_legacy_object_size=display->getMetaClass()->getClassSize();
    native_legacy_parent_match=(display->metaCast("IOMobileFramebufferLegacy") && display->metaCast("IOMobileFramebuffer") && display->metaCast("IOService"));
    native_legacy_stage=3;native_legacy_init_result=display->init();
    unsigned phase=1;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
    if(native_legacy_init_result && (phase==2 || phase==3 || phase==4)) {
        // IOSurface matches the real IOBSD resource, published by bsd_autoconf.
        // Retain this actual object until that normal lifecycle boundary.
        pendingLegacyDisplay=display;return;
    }
    native_legacy_stage=7;display->release();native_legacy_released=1;native_legacy_stage=8;
    native_legacy_display_checkpoint();
}
// The original IOHIDFamily (IOHIDSystem, IOHIDResource and the user clients backboardd connects to) and IOHIDEventDriver: both are
// roots of the OS profile, and backboardd (display and input server) waits for them. disp4 showed backboardd opening the display,
// asking a handful of questions and then sitting in mach_msg for good. No input device is attached yet; this only brings the
// services up. evb10-hid=1 turns it on.
extern "C" void evb10_hid_input_start(IOService *resource);
static void evb10_hid_load(void) {
    unsigned want=0;PE_parse_boot_argn("evb10-hid",&want,sizeof want);
    if(want!=1)return;
    const int family=OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
    int driver=-1;
    if(!family)driver=OSKext::loadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver",true,false,kOSKextExcludeNone,kOSKextExcludeNone,nullptr);
    bool resource=false;
    if(!family) {
        OSDictionary *match=IOService::serviceMatching("IOHIDResource");
        IOService *found=match?IOService::waitForMatchingService(match,5000000000ULL):nullptr;
        resource=found!=nullptr;
        printf("evb10: hid: IOHIDFamily %d IOHIDEventDriver %d IOHIDResource %s\n",family,driver,resource?"found":"missing");
        // evb10-hid-input=1: the owned keyboard + touch screen device on the resource (package/Evb10HidProbe.cpp, package/Evb10Hid)
        if(found)evb10_hid_input_start(found);
        OSSafeReleaseNULL(found);OSSafeReleaseNULL(match);
        return;
    }
    printf("evb10: hid: IOHIDFamily %d IOHIDEventDriver %d IOHIDResource %s\n",family,driver,resource?"found":"missing");
}
void native_legacy_display_late_probe(void);
extern void native_keystore_probe(void);
static void legacy_late_probe_body(void);
void native_legacy_display_late_probe(void) {
    legacy_late_probe_body();
    evb10_display_report("late");
}
// package/Evb10Touch: the touch panel's I2C master and controller, polled from a thread call (evb10-touch=1).
extern "C" void evb10_touch_start(void);
// package/Mali: the Mali-G610 bring-up (mali=<milestone>).
extern "C" void mali_start(void);
static void legacy_late_probe_body(void) {
    native_keystore_probe();
    evb10_hid_load();
    evb10_touch_start();
    mali_start();
    IOService *display=pendingLegacyDisplay;if(!display)return;pendingLegacyDisplay=nullptr;
    native_legacy_surface_status=OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
    OSDictionary *matching=IOService::nameMatching("IOCoreSurfaceRoot");
    IOService *surface=matching?IOService::waitForMatchingService(matching,5000000000ULL):nullptr;
    native_legacy_surface_found=surface!=nullptr;OSSafeReleaseNULL(surface);OSSafeReleaseNULL(matching);
    PE_Video video={};
    if(!native_legacy_surface_status && native_legacy_surface_found && !PE_current_console(&video)
        && video.v_baseAddr && video.v_width && video.v_height && video.v_depth==32
        && !video.v_rotate && video.v_width<=16384 && video.v_height<=16384
        && video.v_rowBytes>=video.v_width*4 && video.v_rowBytes<=UINT64_MAX/video.v_height) {
        native_legacy_video_base=video.v_baseAddr;native_legacy_video_bytes=video.v_rowBytes*video.v_height;
        // A real firmware framebuffer resource, with addresses and extent
        // obtained from the actual GOP handoff. It has no invented compatible,
        // display list, panel timing, GPU/fence completion or vendor registers.
        native_legacy_stage=4;IOPlatformDevice *nub=new IOPlatformDevice;
        native_legacy_nub=(uintptr_t)nub;bool nubAttached=false,nubStarted=false,attached=false,started=false;
        IORegistryEntry *displayTreeParent=nullptr;bool dtAttached=false;
        IOService *parent=IOService::getResourceService();
        IODeviceMemory *memory=IODeviceMemory::withRange(video.v_baseAddr,video.v_rowBytes*video.v_height);
        OSArray *ranges=OSArray::withCapacity(1);
        if(nub && memory && ranges && parent && nub->init()) {
            ranges->setObject(memory);nub->setDeviceMemory(ranges);nub->setName("NativeGOPFramebuffer");
            nubAttached=nub->attach(parent);if(nubAttached)nubStarted=nub->start(parent);
            if(nubStarted) {
                unsigned phase=0;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
                if(phase==4) {
                    // This is the actually implemented GOP display subsystem,
                    // attached to its real IOPlatformDevice memory resource.
                    // No Apple/vendor compatible or panel timing is supplied.
                    displayTreeParent=IORegistryEntry::fromPath("/arm-io",gIODTPlane);
                    const char type[]="display-subsystem";OSData *data=OSData::withBytes(type,sizeof(type));
                    if(displayTreeParent && data && nub->setProperty("device_type",data)) {
                        nub->setName("gop-display",gIODTPlane);dtAttached=nub->attachToParent(displayTreeParent,gIODTPlane);
                    }
                    OSSafeReleaseNULL(data);native_legacy_dt_attached=dtAttached;
                }
                if(phase==3) {
                    IOService *presenter=native_gop_presenter_create();bool presenterAttached=false;
                    if(presenter && presenter->init() && (presenterAttached=presenter->attach(nub)) && presenter->start(nub)) {
                        // Explicit own transport service only. The incomplete
                        // original framebuffer backend is never published.
                        activeGOPPresenter=presenter;activeGOPNub=nub;nub=nullptr;nubAttached=false;nubStarted=false;
                    } else {if(presenterAttached)presenter->detach(nub);OSSafeReleaseNULL(presenter);}
                } else {
                    SInt32 score=0;native_legacy_probe_match=(phase!=4 || dtAttached) && display->probe(nub,&score)==display;
                    if(native_legacy_probe_match)attached=display->attach(nub);
                    if(attached){native_legacy_stage=5;started=display->start(nub);native_legacy_start_result=started;}
                    if(started && phase==4) {
                        activeLegacyDisplay=display;activeGOPNub=nub;native_legacy_stage=10;
                        {
                            unsigned gpu=0;PE_parse_boot_argn("sheng-fb-gpu",&gpu,sizeof gpu);
                            if(gpu==1) {
                                IOService *handoff=native_gop_presenter_create();
                                // Not a child of the display subsystem's node: the original display code enumerates that node's children (int-075/077:
                                // SpringBoard found no display and crashed with the presenter attached there, registered or not).
                                IOService *home=IOService::getResourceService();
                                bool handoffAttached=false;
                                if(handoff && home)native_gop_presenter_set_source(handoff,nub);
                                if(handoff && home && handoff->init() && (handoffAttached=handoff->attach(home)) && handoff->start(home))gpuHandoffPresenter=handoff;
                                else {if(handoffAttached)handoff->detach(home);OSSafeReleaseNULL(handoff);}
                            }
                        }
                        finishUnitScalePolicy(true);
                        {
                            // The UI is drawn into the same frame buffer the console mirror writes characters into: stop the
                            // mirror, or the kernel log scrolls over SpringBoard. The UART keeps the log. evb10-vc-keep=1 keeps it.
                            unsigned keep=0;PE_parse_boot_argn("evb10-vc-keep",&keep,sizeof keep);
                            if(!keep && evb10_vc_mirror) {
                                printf("evb10: display live: the console is no longer mirrored on the screen (evb10-vc-keep=1 keeps it)\n");
                                evb10_vc_mirror=false;
                            }
                        }
                        evb10_fb_writecombine();
                        evb10_display_reporter_start();
                        OSSafeReleaseNULL(displayTreeParent);
                        native_legacy_display_checkpoint();return;
                    }
                }
            }
        }
        native_legacy_stage=6;finishUnitScalePolicy(false);
        if(started)display->stop(nub);if(attached)display->detach(nub);
        if(dtAttached)nub->detachFromParent(displayTreeParent,gIODTPlane);
        if(nubStarted)nub->stop(parent);if(nubAttached)nub->detach(parent);
        OSSafeReleaseNULL(ranges);OSSafeReleaseNULL(memory);OSSafeReleaseNULL(nub);
        OSSafeReleaseNULL(displayTreeParent);
    }
    native_legacy_stage=7;display->release();native_legacy_released=1;native_legacy_stage=8;
    native_legacy_display_checkpoint();
}
}
