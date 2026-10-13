/*
 * DirectSound sketch. The shim will map IDirectSoundBuffer onto the audio
 * layer's ring (vt3_audio.h): Lock/Unlock become writes into the ring, and
 * GetCurrentPosition becomes vt3_audio_played_bytes(). Not implemented yet.
 *
 * As with d3d9.h, the method order is reproduced from memory of the SDK
 * header. Verify it before use.
 */
#ifndef VT3_WIN32_DSOUND_H
#define VT3_WIN32_DSOUND_H

#include "win32/types.h"

#define DSERR_NODRIVER 0x88780078u   /* returned until the shim has a backend */

typedef struct WAVEFORMATEX {
    WORD  wFormatTag;
    WORD  nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD  nBlockAlign;
    WORD  wBitsPerSample;
    WORD  cbSize;
} WAVEFORMATEX;

/* IDirectSoundBuffer: 21 methods, in SDK order. */
typedef struct IDirectSoundBufferVtbl {
    GPTR QueryInterface, AddRef, Release;
    GPTR GetCaps, GetCurrentPosition, GetFormat, GetVolume, GetPan, GetFrequency, GetStatus;
    GPTR Initialize, Lock, Play, SetCurrentPosition, SetFormat, SetVolume, SetPan, SetFrequency;
    GPTR Stop, Unlock, Restore;
} IDirectSoundBufferVtbl;

HRESULT WINAPI DirectSoundCreate(LPCGUID device, GPTR out_ds, LPUNKNOWN outer);

#endif /* VT3_WIN32_DSOUND_H */
