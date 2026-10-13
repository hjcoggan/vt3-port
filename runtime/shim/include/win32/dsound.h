/*
 * DirectSound interface layouts and constants for the shim.
 * The shim will map IDirectSoundBuffer onto the audio layer's ring
 * (vt3_audio.h): Lock/Unlock become ring writes, and GetCurrentPosition
 * becomes vt3_audio_played_bytes(). Not implemented yet.
 *
 * Layouts checked against the mingw-w64 14.0.0 headers: tests/sdk_layout_check.c
 * asserts every slot offset and constant, and tests/layout_check.c checks these
 * declarations at host build time. Slot order is the SDK's.
 */
#ifndef VT3_WIN32_DSOUND_H
#define VT3_WIN32_DSOUND_H

#include "win32/types.h"

/* DSERR_NODRIVER = MAKE_DSHRESULT(120). Returned until the shim has a backend. */
#define DSERR_NODRIVER 0x88780078u

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
