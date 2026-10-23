/*
 * DirectSound interface layouts and constants for the shim. The objects are silent
 * for now (dsound_stub.c): buffers hold PCM in guest memory and the play cursor
 * follows wall-clock time. The audio layer (vt3_audio.h) is not connected yet.
 *
 * Layouts checked against the mingw-w64 14.0.0 headers: tests/sdk_layout_check.c
 * asserts every slot offset and constant, and tests/layout_check.c checks these
 * declarations at host build time. Slot order is the SDK's.
 */
#ifndef VT3_WIN32_DSOUND_H
#define VT3_WIN32_DSOUND_H

#include "win32/types.h"

/* HRESULTs, flags and limits the shim returns or reads (mingw-w64 14.0.0 dsound.h). */
#define DS_OK                    0x00000000u
#define DSERR_NODRIVER           0x88780078u   /* MAKE_DSHRESULT(120) */
#define DSERR_INVALIDPARAM       0x80070057u   /* E_INVALIDARG */
#define DSERR_INVALIDCALL        0x88780032u   /* MAKE_DSHRESULT(50) */
#define DSERR_UNSUPPORTED        0x80004001u   /* E_NOTIMPL */
#define DSERR_NOAGGREGATION      0x80040110u   /* CLASS_E_NOAGGREGATION */
#define DSERR_ALREADYINITIALIZED 0x88780082u   /* MAKE_DSHRESULT(130) */
#define DSERR_BUFFERLOST         0x88780096u   /* MAKE_DSHRESULT(150) */

#define DSBPLAY_LOOPING          0x00000001u
#define DSBSTATUS_PLAYING        0x00000001u
#define DSBSTATUS_BUFFERLOST     0x00000002u
#define DSBSTATUS_LOOPING        0x00000004u
#define DSBLOCK_FROMWRITECURSOR  0x00000001u
#define DSBLOCK_ENTIREBUFFER     0x00000002u
#define DSBCAPS_PRIMARYBUFFER    0x00000001u
#define DSBCAPS_LOCSOFTWARE      0x00000008u
#define DSBCAPS_CTRLDEFAULT      0x000000E0u
#define DSBVOLUME_MAX            0
#define DSBVOLUME_MIN            (-10000)
#define DSBPAN_LEFT              (-10000)
#define DSBPAN_RIGHT             10000
#define DSBFREQUENCY_ORIGINAL    0
#define DSBFREQUENCY_MIN         100
#define DSBFREQUENCY_MAX         200000
#define DSCAPS_EMULDRIVER        0x00000020u
#define DSSPEAKER_STEREO         4u

/* WAVEFORMATEX, as in mmreg.h (packed to 1 there). 18 bytes. */
#pragma pack(push, 1)
typedef struct WAVEFORMATEX {
    WORD  wFormatTag;
    WORD  nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD  nBlockAlign;
    WORD  wBitsPerSample;
    WORD  cbSize;
} WAVEFORMATEX;
#pragma pack(pop)

/* IDirectSound: 11 slots, in the SDK's order. Slot N is at byte offset 4*N. */
typedef struct IDirectSoundVtbl {
    GPTR QueryInterface;              /* 0 */
    GPTR AddRef;                      /* 1 */
    GPTR Release;                     /* 2 */
    GPTR CreateSoundBuffer;           /* 3 */
    GPTR GetCaps;                     /* 4 */
    GPTR DuplicateSoundBuffer;        /* 5 */
    GPTR SetCooperativeLevel;         /* 6 */
    GPTR Compact;                     /* 7 */
    GPTR GetSpeakerConfig;            /* 8 */
    GPTR SetSpeakerConfig;            /* 9 */
    GPTR Initialize;                  /* 10 */
} IDirectSoundVtbl;

/* IDirectSoundBuffer: 21 slots, in the SDK's order. Slot N is at byte offset 4*N. */
typedef struct IDirectSoundBufferVtbl {
    GPTR QueryInterface;              /* 0 */
    GPTR AddRef;                      /* 1 */
    GPTR Release;                     /* 2 */
    GPTR GetCaps;                     /* 3 */
    GPTR GetCurrentPosition;          /* 4 */
    GPTR GetFormat;                   /* 5 */
    GPTR GetVolume;                   /* 6 */
    GPTR GetPan;                      /* 7 */
    GPTR GetFrequency;                /* 8 */
    GPTR GetStatus;                   /* 9 */
    GPTR Initialize;                  /* 10 */
    GPTR Lock;                        /* 11 */
    GPTR Play;                        /* 12 */
    GPTR SetCurrentPosition;          /* 13 */
    GPTR SetFormat;                   /* 14 */
    GPTR SetVolume;                   /* 15 */
    GPTR SetPan;                      /* 16 */
    GPTR SetFrequency;                /* 17 */
    GPTR Stop;                        /* 18 */
    GPTR Unlock;                      /* 19 */
    GPTR Restore;                     /* 20 */
} IDirectSoundBufferVtbl;

HRESULT WINAPI DirectSoundCreate(LPCGUID device, GPTR out_ds, LPUNKNOWN outer);

/* ---- mixer core (dsound_stub.c), exposed for the self-test --------------------
 * The per-buffer renderer behind the mixer thread: fixed-point phase stepping with
 * linear interpolation (a source that is not the output rate is resampled smoothly,
 * not sample-held, which aliases as a hard, gargly edge), and a Q15 gate that fades
 * the blocks the game has not written out over VT3_MIX_RAMP_FRAMES instead of
 * cutting to silence (a click) or replaying stale audio (see e8d1a37). */

#define VT3_MIX_FRESH_BLOCK  256u        /* gate granularity: one flag per 256 bytes */
#define VT3_MIX_GATE_MAX     32768       /* Q15: full level */
#define VT3_MIX_RAMP_FRAMES  96u         /* gate ramp length, 2 ms at 48 kHz */

typedef struct ds_mix_state {
    uint64_t phase;      /* Q32 source frame index, absolute from the buffer's start */
    uint32_t last_blk;   /* FRESH_BLOCK block the last frame read, or UINT32_MAX */
    int32_t gate;        /* Q15 output gate: 0 silent, VT3_MIX_GATE_MAX full */
} ds_mix_state;

/* Q32 phase step for rate_hz source frames per second against the 48 kHz output. */
uint64_t ds_mix_step(uint32_t rate_hz);
/* DirectSound volume (hundredths of a dB, -10000..0) and pan (-10000..10000) as channel
 * gains: DirectSound attenuates the far channel linearly and leaves the near one alone. */
double ds_mix_volume(int32_t hundredths_db);
void   ds_mix_pan(int32_t pan, double *gl, double *gr);
/* Adds `frames` output frames of `data` (bytes of nch-channel, bits-bit PCM at `align`
 * bytes a frame) to the interleaved stereo accumulator acc, starting at st->phase and
 * advancing it by step_q32 an output frame. fresh, when not NULL, is one flag per
 * VT3_MIX_FRESH_BLOCK bytes set by the game's writes and cleared once played: an
 * unwritten block fades the gate out, so a late writer costs a dip, not a click or a
 * repeat. Returns the frames rendered; fewer than `frames` at the end of a non-looping
 * buffer. The accumulator is not cleared, so several buffers sum into it. */
uint32_t ds_mix_render(int32_t *acc, uint32_t frames, const uint8_t *data, uint32_t bytes,
                       uint32_t nch, uint32_t bits, uint32_t align, int looping,
                       uint8_t *fresh, uint64_t step_q32, double gl, double gr,
                       ds_mix_state *st);

#endif /* VT3_WIN32_DSOUND_H */
