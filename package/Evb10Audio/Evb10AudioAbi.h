/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The interface between the EVB10 audio service (package/Evb10Audio/Evb10Audio.cpp, IOKit class Evb10Audio) and the audio server plug-in that
 * iPadOS loads (hal/Evb10AudioPlugIn.c). The plug-in opens the service, maps three memory types and starts and stops each direction.
 *
 * Derived from UniXNU-sheng's ShengAudioAbi.h (BSD-2-Clause-Patent, Copyright (c) 2026, UniXNU contributors), which carries the same idea for a
 * DSP: here the "DSP" is the I2S0 controller and its FIFOs, which the service's thread keeps filled.
 *
 * Memory type 0 is the control page; 1 the playback ring; 2 the capture ring. Rings hold 16-bit little-endian interleaved samples, ring_frames
 * frames each. Sample time t lives at frame t % ring_frames. One timeline serves both directions and the whole boot: sample time 0 is the
 * frame on the wire at host time anchor_host, and one frame lasts ticks_per_frame_q32 / 2^32 host ticks (the I2S clock and the host timer both
 * come from the 24 MHz crystal, so the two do not drift). The service takes playback frame t from the ring when it is at most fifo_frames
 * ahead of the wire, clears it, and puts captured frame t into the capture ring as it arrives, so the plug-in writes playback at least
 * lead_frames ahead of the play position and reads capture frames that are older than that. */
#ifndef EVB10_AUDIO_ABI_H
#define EVB10_AUDIO_ABI_H
#include <stdint.h>

#define EVB10_AUDIO_CLASS "Evb10Audio"
#define EVB10_AUDIO_MAGIC 0x45313041u         /* 'E10A' */
#define EVB10_AUDIO_VERSION 1u
#define EVB10_AUDIO_RATE 48000u
#define EVB10_AUDIO_OUT_CHANNELS 2u
#define EVB10_AUDIO_IN_CHANNELS 1u
#define EVB10_AUDIO_PERIOD_FRAMES 480u        /* 10 ms: the unit of the plug-in's safety offset */
#define EVB10_AUDIO_RING_FRAMES 65536u        /* 1.37 s: room for the audio server's late IO cycles */

enum {Evb10AudioMemoryControl = 0, Evb10AudioMemoryOutput = 1, Evb10AudioMemoryInput = 2};
enum {Evb10AudioStartOutput = 0, Evb10AudioStopOutput = 1, Evb10AudioStartInput = 2, Evb10AudioStopInput = 3};
enum {Evb10AudioOutputRunning = 1u << 0, Evb10AudioInputRunning = 1u << 1, Evb10AudioHardwareOn = 1u << 2};

typedef struct {
    uint32_t magic, version;
    uint32_t rate, out_channels, in_channels, period_frames, ring_frames, lead_frames;
    volatile uint32_t state;                 /* Evb10Audio*Running bits (requested) and HardwareOn (the I2S clocks run) */
    uint32_t fifo_frames;                    /* how far ahead of the wire the service fills the transmit FIFO */
    uint64_t ticks_per_frame_q32;            /* host ticks per frame, 32.32 fixed point */
    volatile uint64_t anchor_host;           /* host time of sample time 0 (set once, when the service starts) */
    volatile uint64_t out_frames;            /* the sample time after the last frame the service took from the playback ring */
    volatile uint64_t in_frames;             /* the sample time after the last frame the service put into the capture ring */
    volatile uint32_t out_late;              /* times the service found the transmit FIFO run dry and had to skip frames to catch up */
    volatile uint32_t in_late;               /* receive FIFO overruns */
    volatile uint32_t out_seq, in_seq;       /* unused (the timeline is never re-anchored); kept at 0 */
    volatile uint64_t out_written;           /* the plug-in: the sample time after the last frame it wrote */
    volatile uint64_t in_written;            /* the service: the sample time after the last captured frame in the ring */
    volatile uint64_t out_written_host;      /* the plug-in: host time of its last write */
    volatile uint32_t out_holes;             /* unused */
    volatile uint32_t out_skipped_frames;    /* frames of the playback ring dropped because the wire had passed them */
    volatile uint32_t out_peak, in_peak;     /* the largest sample taken from / put into the rings since the last start (16-bit magnitude) */
    volatile uint32_t wakeups, max_gap_us;   /* the service thread's ticks and the longest time between two */
} Evb10AudioControl;

/* Second half of the control page: the plug-in records what the audio server asked for and it does not answer (selector, object id
 * << 24 | scope fourcc's first byte), so the kernel log names them without the unified log. */
#define EVB10_AUDIO_DIAG_OFFSET 2048u
#define EVB10_AUDIO_DIAG_SLOTS 32u
typedef struct {
    volatile uint32_t count, loads;
    volatile uint32_t selector[EVB10_AUDIO_DIAG_SLOTS], where[EVB10_AUDIO_DIAG_SLOTS];
    volatile uint32_t volume_milli, volume_sets;  /* the output volume (scalar x 1000) and how often the audio server set it */
    volatile uint32_t mix_peak;                   /* the largest output sample before the volume, x 32767 */
    volatile uint32_t in_misses;                  /* input reads that asked for frames the service had not captured yet */
    /* How far ahead of the play position the audio server writes (output time minus now, frames) and how far behind it reads, the
     * least and most of each, and the frames of the last write. */
    volatile int32_t ahead_min, ahead_max, behind_min, behind_max;
    volatile uint32_t io_frames, writes;
    /* Writes that began inside frames already written (another IO context of the audio server mixing into the same ring), and the most
     * frames such a write went back. */
    volatile uint32_t overlaps, overlap_max;
    /* Every frame written; writes that began past the end of the one before (a skipped stretch) and the frames skipped; the time from
     * one write to the next in buckets below 15, 25, 40, 80 and 160 ms and above (one IO cycle of 1024 frames is 21.3 ms). */
    volatile uint32_t frames_total, gaps, gap_frames;
    volatile uint32_t interval[6];
    volatile uint64_t last_end, last_host;
    volatile int32_t skew_min, skew_max;          /* the audio server's current sample time minus the timeline's, least and most */
    /* The audio server's IO thread as the plug-in sees it: current and base priority, scheduling policy, whether it has a time constraint
     * and that constraint's period, computation and constraint (host ticks); the lowest current priority seen. */
    volatile int32_t io_pri, io_base, io_policy, io_tc, io_pri_min;
    volatile uint32_t io_period, io_computation, io_constraint;
    /* CoreAudio's host clock frequency (Hz) as AudioGetHostClockFrequency() returns it in the audio server, and how the plug-in looked it
     * up (0 done, -1 the symbol was not found). The board's timer is 24 MHz, which is what CoreAudio assumes, so nothing is changed. */
    volatile uint32_t host_clock;
    volatile int32_t host_clock_result;
} Evb10AudioDiag;
/* After it, every distinct property access in the order the audio server made it: selector, then object << 24 | scope's first byte << 16 |
 * kind (1 set, 0 get, 4 failing call of the routing layer, 8 throw) | 2 if it failed. */
#define EVB10_AUDIO_TRACE_OFFSET 2560u
#define EVB10_AUDIO_TRACE_SLOTS 128u
typedef struct {
    volatile uint32_t count;
    volatile uint32_t entry[EVB10_AUDIO_TRACE_SLOTS][2];
} Evb10AudioTrace;

#ifdef __cplusplus
#define EVB10_AUDIO_ASSERT static_assert
#else
#define EVB10_AUDIO_ASSERT _Static_assert
#endif
EVB10_AUDIO_ASSERT(sizeof(Evb10AudioControl) <= EVB10_AUDIO_DIAG_OFFSET, "the control block must end before the diagnostics");
EVB10_AUDIO_ASSERT(EVB10_AUDIO_DIAG_OFFSET + sizeof(Evb10AudioDiag) <= EVB10_AUDIO_TRACE_OFFSET, "the diagnostics must end before the trace");
EVB10_AUDIO_ASSERT(EVB10_AUDIO_TRACE_OFFSET + sizeof(Evb10AudioTrace) <= 4096, "the control page is one 4 KiB page");

#endif
