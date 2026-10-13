/*
 * DirectInput 8 interface layouts and entry point for the shim.
 * Not implemented: DirectInput8Create returns E_NOTIMPL. The shim will
 * later feed device state into vt3_input.h, in place of SDL.
 *
 * Layouts checked against the mingw-w64 14.0.0 headers: tests/sdk_layout_check.c
 * asserts every slot offset and constant, and tests/layout_check.c checks these
 * declarations at host build time. Slot order is the SDK's.
 *
 * The game is expected to ask for the 'A' interfaces (IDirectInput8A). The
 * IID checks in DirectInput8Create are not built yet.
 */
#ifndef VT3_WIN32_DINPUT_H
#define VT3_WIN32_DINPUT_H

#include "win32/types.h"

/* DIRECTINPUT_VERSION the layouts were checked against. */
#define DIRECTINPUT_VERSION 0x0800

/* IDirectInput8A: 11 slots, in the SDK's order. Slot N is at byte offset 4*N. */
typedef struct IDirectInput8AVtbl {
    GPTR QueryInterface;              /* 0 */
    GPTR AddRef;                      /* 1 */
    GPTR Release;                     /* 2 */
    GPTR CreateDevice;                /* 3 */
    GPTR EnumDevices;                 /* 4 */
    GPTR GetDeviceStatus;             /* 5 */
    GPTR RunControlPanel;             /* 6 */
    GPTR Initialize;                  /* 7 */
    GPTR FindDevice;                  /* 8 */
    GPTR EnumDevicesBySemantics;      /* 9 */
    GPTR ConfigureDevices;            /* 10 */
} IDirectInput8AVtbl;

/* IDirectInputDevice8A: 32 slots, in the SDK's order. Slot N is at byte offset 4*N. */
typedef struct IDirectInputDevice8AVtbl {
    GPTR QueryInterface;              /* 0 */
    GPTR AddRef;                      /* 1 */
    GPTR Release;                     /* 2 */
    GPTR GetCapabilities;             /* 3 */
    GPTR EnumObjects;                 /* 4 */
    GPTR GetProperty;                 /* 5 */
    GPTR SetProperty;                 /* 6 */
    GPTR Acquire;                     /* 7 */
    GPTR Unacquire;                   /* 8 */
    GPTR GetDeviceState;              /* 9 */
    GPTR GetDeviceData;               /* 10 */
    GPTR SetDataFormat;               /* 11 */
    GPTR SetEventNotification;        /* 12 */
    GPTR SetCooperativeLevel;         /* 13 */
    GPTR GetObjectInfo;               /* 14 */
    GPTR GetDeviceInfo;               /* 15 */
    GPTR RunControlPanel;             /* 16 */
    GPTR Initialize;                  /* 17 */
    GPTR CreateEffect;                /* 18 */
    GPTR EnumEffects;                 /* 19 */
    GPTR GetEffectInfo;               /* 20 */
    GPTR GetForceFeedbackState;       /* 21 */
    GPTR SendForceFeedbackCommand;    /* 22 */
    GPTR EnumCreatedEffectObjects;    /* 23 */
    GPTR Escape;                      /* 24 */
    GPTR Poll;                        /* 25 */
    GPTR SendDeviceData;              /* 26 */
    GPTR EnumEffectsInFile;           /* 27 */
    GPTR WriteEffectToFile;           /* 28 */
    GPTR BuildActionMap;              /* 29 */
    GPTR SetActionMap;                /* 30 */
    GPTR GetImageInfo;                /* 31 */
} IDirectInputDevice8AVtbl;

HRESULT WINAPI DirectInput8Create(GPTR hinst, DWORD version, LPCGUID riid, GPTR out, LPUNKNOWN outer);

#endif /* VT3_WIN32_DINPUT_H */
