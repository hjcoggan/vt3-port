/*
 * DirectInput 8 for the shim: interface layouts, constants and entry points.
 * Implemented in src/dinput8.c over the SDL input layer (vt3_input.h).
 *
 * Layouts checked against the mingw-w64 14.0.0 headers: tests/sdk_layout_check.c
 * asserts every slot offset and constant, and tests/layout_check.c checks these
 * declarations at host build time. Slot order is the SDK's.
 *
 * The game asks for the 'A' interface (IDirectInput8A). Every method is a WINAPI
 * function, bound by name into the guest vtables that dinput8.c builds.
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

/* Results. Values are HRESULTs (DIERR_* as in the SDK; DI_NOEFFECT is S_FALSE). */
#define DI_OK                      0u
#define DI_NOEFFECT                1u
#define DIERR_NOTFOUND             0x80070002u
#define DIERR_INVALIDPARAM         0x80070057u
#define DIERR_OUTOFMEMORY          0x8007000Eu
#define DIERR_NOTACQUIRED          0x8007000Cu
#define DIERR_NOTINITIALIZED       0x80070015u
#define DIERR_INPUTLOST            0x8007001Eu
#define DIERR_ACQUIRED             0x800700AAu
#define DIERR_ALREADYINITIALIZED   0x800704DFu
#define DIERR_UNSUPPORTED          0x80004001u
#define DIERR_NOINTERFACE          0x80004002u
#define DIERR_NOTBUFFERED          0x80040207u
#define DIERR_DEVICENOTREG         0x80040154u
#define DIERR_NOAGGREGATION        0x80040110u

#define DIENUM_STOP                0
#define DIENUM_CONTINUE            1

/* Device classes and types (DI8DEVCLASS_* selects; DI8DEVTYPE_* describes). */
#define DI8DEVCLASS_ALL            0
#define DI8DEVCLASS_KEYBOARD       3
#define DI8DEVCLASS_GAMECTRL       4
#define DI8DEVTYPE_KEYBOARD        0x13
#define DI8DEVTYPE_JOYSTICK        0x14
#define DI8DEVTYPE_GAMEPAD         0x15

#define DIEDFL_ALLDEVICES          0x00000000u
#define DIEDFL_ATTACHEDONLY        0x00000001u

#define DIDC_ATTACHED              0x00000001u
#define DIDC_POLLEDDEVICE          0x00000002u

/* Object types in a data format (DIDFT_*). The type's instance is in bits 8..23. */
#define DIDFT_RELAXIS              0x00000001u
#define DIDFT_ABSAXIS              0x00000002u
#define DIDFT_AXIS                 0x00000003u
#define DIDFT_PSHBUTTON            0x00000004u
#define DIDFT_TGLBUTTON            0x00000008u
#define DIDFT_BUTTON               0x0000000Cu
#define DIDFT_POV                  0x00000010u
#define DIDFT_NODATA               0x00000080u
#define DIDFT_OPTIONAL             0x80000000u

#define DIGDD_PEEK                 0x00000001u

/* Properties. The GUID argument of GetProperty/SetProperty is MAKEDIPROP(n) = n. */
#define DIPROP_BUFFERSIZE          1
#define DIPROP_AXISMODE            2
#define DIPROP_GRANULARITY         3
#define DIPROP_RANGE               4
#define DIPROP_DEADZONE            5
#define DIPROP_SATURATION          6
#define DIPROPAXISMODE_ABS         0
#define DIPROPAXISMODE_REL         1

/* Guest struct sizes (i686). DIDEVICEINSTANCE is the 580-byte DX8 form. */
#define DIDEVICEINSTANCE_SIZE      580
#define DIDEVCAPS_SIZE             44
#define DIDEVICEOBJECTDATA_SIZE    16
#define DIOBJECTDATAFORMAT_SIZE    16
#define DIDATAFORMAT_SIZE          24
#define DIPROPHEADER_SIZE          16

HRESULT WINAPI DirectInput8Create(GPTR hinst, DWORD version, LPCGUID riid, GPTR out, LPUNKNOWN outer);

/* IDirectInput8A methods. Pointer arguments are guest addresses. */
HRESULT WINAPI IDirectInput8A_QueryInterface(GPTR This, GPTR riid, GPTR ppvObj);
DWORD   WINAPI IDirectInput8A_AddRef(GPTR This);
DWORD   WINAPI IDirectInput8A_Release(GPTR This);
HRESULT WINAPI IDirectInput8A_CreateDevice(GPTR This, GPTR rguid, GPTR lplpDirectInputDevice, GPTR pUnkOuter);
HRESULT WINAPI IDirectInput8A_EnumDevices(GPTR This, DWORD dwDevType, GPTR lpCallback, GPTR pvRef, DWORD dwFlags);
HRESULT WINAPI IDirectInput8A_GetDeviceStatus(GPTR This, GPTR rguidInstance);
HRESULT WINAPI IDirectInput8A_RunControlPanel(GPTR This, GPTR hwndOwner, DWORD dwFlags);
HRESULT WINAPI IDirectInput8A_Initialize(GPTR This, GPTR hinst, DWORD dwVersion);
HRESULT WINAPI IDirectInput8A_FindDevice(GPTR This, GPTR rguidClass, GPTR ptszName, GPTR pguidInstance);
HRESULT WINAPI IDirectInput8A_EnumDevicesBySemantics(GPTR This, GPTR ptszUserName, GPTR lpdiActionFormat,
                                                     GPTR lpCallback, GPTR pvRef, DWORD dwFlags);
HRESULT WINAPI IDirectInput8A_ConfigureDevices(GPTR This, GPTR lpdiCallback, GPTR lpdiCP, DWORD dwFlags,
                                               GPTR pvRefData);

/* IDirectInputDevice8A methods. */
HRESULT WINAPI IDirectInputDevice8A_QueryInterface(GPTR This, GPTR riid, GPTR ppvObj);
DWORD   WINAPI IDirectInputDevice8A_AddRef(GPTR This);
DWORD   WINAPI IDirectInputDevice8A_Release(GPTR This);
HRESULT WINAPI IDirectInputDevice8A_GetCapabilities(GPTR This, GPTR lpDIDevCaps);
HRESULT WINAPI IDirectInputDevice8A_EnumObjects(GPTR This, GPTR lpCallback, GPTR pvRef, DWORD dwFlags);
HRESULT WINAPI IDirectInputDevice8A_GetProperty(GPTR This, GPTR rguidProp, GPTR pdiph);
HRESULT WINAPI IDirectInputDevice8A_SetProperty(GPTR This, GPTR rguidProp, GPTR pdiph);
HRESULT WINAPI IDirectInputDevice8A_Acquire(GPTR This);
HRESULT WINAPI IDirectInputDevice8A_Unacquire(GPTR This);
HRESULT WINAPI IDirectInputDevice8A_GetDeviceState(GPTR This, DWORD cbData, GPTR lpvData);
HRESULT WINAPI IDirectInputDevice8A_GetDeviceData(GPTR This, DWORD cbObjectData, GPTR rgdod, GPTR pdwInOut,
                                                  DWORD dwFlags);
HRESULT WINAPI IDirectInputDevice8A_SetDataFormat(GPTR This, GPTR lpdf);
HRESULT WINAPI IDirectInputDevice8A_SetEventNotification(GPTR This, GPTR hEvent);
HRESULT WINAPI IDirectInputDevice8A_SetCooperativeLevel(GPTR This, GPTR hwnd, DWORD dwFlags);
HRESULT WINAPI IDirectInputDevice8A_GetObjectInfo(GPTR This, GPTR pdidoi, DWORD dwObj, DWORD dwHow);
HRESULT WINAPI IDirectInputDevice8A_GetDeviceInfo(GPTR This, GPTR pdidi);
HRESULT WINAPI IDirectInputDevice8A_RunControlPanel(GPTR This, GPTR hwndOwner, DWORD dwFlags);
HRESULT WINAPI IDirectInputDevice8A_Initialize(GPTR This, GPTR hinst, DWORD dwVersion, GPTR rguid);
HRESULT WINAPI IDirectInputDevice8A_CreateEffect(GPTR This, GPTR rguid, GPTR lpeff, GPTR ppdeff, GPTR pUnkOuter);
HRESULT WINAPI IDirectInputDevice8A_EnumEffects(GPTR This, GPTR lpCallback, GPTR pvRef, DWORD dwEffType);
HRESULT WINAPI IDirectInputDevice8A_GetEffectInfo(GPTR This, GPTR pdei, GPTR rguid);
HRESULT WINAPI IDirectInputDevice8A_GetForceFeedbackState(GPTR This, GPTR pdwOut);
HRESULT WINAPI IDirectInputDevice8A_SendForceFeedbackCommand(GPTR This, DWORD dwFlags);
HRESULT WINAPI IDirectInputDevice8A_EnumCreatedEffectObjects(GPTR This, GPTR lpCallback, GPTR pvRef, DWORD fl);
HRESULT WINAPI IDirectInputDevice8A_Escape(GPTR This, GPTR lpEscape);
HRESULT WINAPI IDirectInputDevice8A_Poll(GPTR This);
HRESULT WINAPI IDirectInputDevice8A_SendDeviceData(GPTR This, DWORD cbObjectData, GPTR rgdod, GPTR pdwInOut,
                                                   DWORD fl);
HRESULT WINAPI IDirectInputDevice8A_EnumEffectsInFile(GPTR This, GPTR lpszFileName, GPTR pec, GPTR pvRef,
                                                      DWORD dwFlags);
HRESULT WINAPI IDirectInputDevice8A_WriteEffectToFile(GPTR This, GPTR lpszFileName, DWORD dwEntries,
                                                      GPTR rgDiFileEft, DWORD dwFlags);
HRESULT WINAPI IDirectInputDevice8A_BuildActionMap(GPTR This, GPTR lpdiaf, GPTR lpszUserName, DWORD dwFlags);
HRESULT WINAPI IDirectInputDevice8A_SetActionMap(GPTR This, GPTR lpdiaf, GPTR lpszUserName, DWORD dwFlags);
HRESULT WINAPI IDirectInputDevice8A_GetImageInfo(GPTR This, GPTR lpdiDevImageInfoHeader);

#endif /* VT3_WIN32_DINPUT_H */
