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

#endif /* VT3_WIN32_DSOUND_H */
