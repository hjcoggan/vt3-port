/*
 * DirectInput 8 for a keyboard and one gamepad, over the SDL input layer (vt3_input.h).
 * IDirectInput8A and IDirectInputDevice8A are guest COM objects (D3D9_KIND_DINPUT and
 * D3D9_KIND_DIDEVICE). Their vtables are built from the names in this file, so each slot
 * is bound to the WINAPI function of the same name. The slot order is the one in win32/dinput.h.
 *
 * Devices:
 *   GUID_SysKeyboard  DI8DEVTYPE_KEYBOARD. Always present.
 *   GUID_Joystick     DI8DEVTYPE_GAMEPAD. Present while a pad is connected.
 *
 * A device reports the data format the game gave to SetDataFormat. Objects are placed by byte
 * offset, as in c_dfDIKeyboard and DIJOYSTATE:
 *   keyboard: offset = DIK code, byte 0x80 while the key is down.
 *   gamepad:  offsets 0..28 are LONG axes X, Y, Z, Rx, Ry, Rz, slider0, slider1;
 *             offsets 32..44 are POV (DWORD, hundredths of a degree, -1 when centred);
 *             offsets 48 on are rgbButtons (BYTE, 0x80 while down).
 *
 * Gamepad layout, ASSUMED (verify against the game's own defaults):
 *   X, Y: left stick. Rx, Ry: right stick. Z, Rz: centred.
 *   slider0, slider1: left and right trigger. POV: D-pad.
 *   buttons 0..10: A, B, X, Y, LB, RB, Back, Start, LS, RS, Guide.
 *   Axes report 0..65535 with the centre at the middle. DIPROP_RANGE changes that range.
 *
 * Not implemented: force-feedback effects, action maps, SetEventNotification. EnumObjects
 * reports no objects. GetDeviceData reports keyboard transitions only; the gamepad is not
 * buffered. Unimplemented methods return DIERR_UNSUPPORTED, with no output written.
 *
 * Guest layouts (i686; offsets from the SDK):
 *   DIDATAFORMAT         dwSize@0 dwObjSize@4 dwFlags@8 dwDataSize@12 dwNumObjs@16 rgodf@20
 *   DIOBJECTDATAFORMAT   pguid@0 dwOfs@4 dwType@8 dwFlags@12
 *   DIDEVICEOBJECTDATA   dwOfs@0 dwData@4 dwTimeStamp@8 dwSequence@12
 *   DIPROPHEADER         dwSize@0 dwHeaderSize@4 dwObj@8 dwHow@12; value at @16 (DIPROPRANGE: lMax@20)
 *   DIDEVCAPS            dwSize@0 dwFlags@4 dwDevType@8 dwAxes@12 dwButtons@16 dwPOVs@20 (44 bytes)
 *   DIDEVICEINSTANCE     dwSize@0 guidInstance@4 guidProduct@20 dwDevType@36 name@40
 *                        productName@300 guidFFDriver@560 wUsagePage@576 wUsage@578 (580 bytes)
 */
#include "d3d9_internal.h"
#include "vt3/vt3_input.h"
#include "vt3/vt3_log.h"
#include "win32/dinput.h"

#include <stdlib.h>
#include <string.h>

#define DI_MAX_OBJ 512
#define DI_MAX_BUFSIZE 4096u

typedef struct di_guid {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
} di_guid;
_Static_assert(sizeof(di_guid) == 16, "GUID must be 16 bytes");

static const di_guid G_IUNKNOWN    = { 0x00000000u, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const di_guid G_IDI8A       = { 0xBF798030u, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
static const di_guid G_IDID8A      = { 0x54D41080u, 0xDC15, 0x4833, { 0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79 } };
static const di_guid G_SYSKEYBOARD = { 0x6F1D2B61u, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const di_guid G_JOYSTICK    = { 0x6F1D2B70u, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

/* ---- keys ----------------------------------------------------------------- */

/* HID keyboard usage (the SDL scancode) -> DIK code. Both name the physical key position. */
typedef struct key_map {
    uint8_t usage, dik;
} key_map;

static const key_map k_keys[] = {
    { 0x04, 0x1E }, { 0x05, 0x30 }, { 0x06, 0x2E }, { 0x07, 0x20 },     /* A B C D */
    { 0x08, 0x12 }, { 0x09, 0x21 }, { 0x0A, 0x22 }, { 0x0B, 0x23 },     /* E F G H */
    { 0x0C, 0x17 }, { 0x0D, 0x24 }, { 0x0E, 0x25 }, { 0x0F, 0x26 },     /* I J K L */
    { 0x10, 0x32 }, { 0x11, 0x31 }, { 0x12, 0x18 }, { 0x13, 0x19 },     /* M N O P */
    { 0x14, 0x10 }, { 0x15, 0x13 }, { 0x16, 0x1F }, { 0x17, 0x14 },     /* Q R S T */
    { 0x18, 0x16 }, { 0x19, 0x2F }, { 0x1A, 0x11 }, { 0x1B, 0x2D },     /* U V W X */
    { 0x1C, 0x15 }, { 0x1D, 0x2C },                                     /* Y Z */
    { 0x1E, 0x02 }, { 0x1F, 0x03 }, { 0x20, 0x04 }, { 0x21, 0x05 },     /* 1 2 3 4 */
    { 0x22, 0x06 }, { 0x23, 0x07 }, { 0x24, 0x08 }, { 0x25, 0x09 },     /* 5 6 7 8 */
    { 0x26, 0x0A }, { 0x27, 0x0B },                                     /* 9 0 */
    { 0x28, 0x1C }, { 0x29, 0x01 }, { 0x2A, 0x0E }, { 0x2B, 0x0F },     /* Return Escape Backspace Tab */
    { 0x2C, 0x39 }, { 0x2D, 0x0C }, { 0x2E, 0x0D }, { 0x2F, 0x1A },     /* Space - = [ */
    { 0x30, 0x1B }, { 0x31, 0x2B }, { 0x33, 0x27 }, { 0x34, 0x28 },     /* ] \ ; ' */
    { 0x35, 0x29 }, { 0x36, 0x33 }, { 0x37, 0x34 }, { 0x38, 0x35 },     /* ` , . / */
    { 0x39, 0x3A },                                                     /* CapsLock */
    { 0x3A, 0x3B }, { 0x3B, 0x3C }, { 0x3C, 0x3D }, { 0x3D, 0x3E },     /* F1..F4 */
    { 0x3E, 0x3F }, { 0x3F, 0x40 }, { 0x40, 0x41 }, { 0x41, 0x42 },     /* F5..F8 */
    { 0x42, 0x43 }, { 0x43, 0x44 }, { 0x44, 0x57 }, { 0x45, 0x58 },     /* F9 F10 F11 F12 */
    { 0x46, 0xB7 }, { 0x47, 0x46 }, { 0x48, 0xC5 }, { 0x49, 0xD2 },     /* PrtSc ScrLk Pause Insert */
    { 0x4A, 0xC7 }, { 0x4B, 0xC9 }, { 0x4C, 0xD3 }, { 0x4D, 0xCF },     /* Home PgUp Delete End */
    { 0x4E, 0xD1 }, { 0x4F, 0xCD }, { 0x50, 0xCB }, { 0x51, 0xD0 },     /* PgDn Right Left Down */
    { 0x52, 0xC8 },                                                     /* Up */
    { 0x53, 0x45 }, { 0x54, 0xB5 }, { 0x55, 0x37 }, { 0x56, 0x4A },     /* NumLock KP/ KP* KP- */
    { 0x57, 0x4E }, { 0x58, 0x9C }, { 0x59, 0x4F }, { 0x5A, 0x50 },     /* KP+ KPEnter KP1 KP2 */
    { 0x5B, 0x51 }, { 0x5C, 0x4B }, { 0x5D, 0x4C }, { 0x5E, 0x4D },     /* KP3 KP4 KP5 KP6 */
    { 0x5F, 0x47 }, { 0x60, 0x48 }, { 0x61, 0x49 }, { 0x62, 0x52 },     /* KP7 KP8 KP9 KP0 */
    { 0x63, 0x53 },                                                     /* KP. */
    { 0xE0, 0x1D }, { 0xE1, 0x2A }, { 0xE2, 0x38 }, { 0xE3, 0xDB },     /* LCtrl LShift LAlt LGUI */
    { 0xE4, 0x9D }, { 0xE5, 0x36 }, { 0xE6, 0xB8 }, { 0xE7, 0xDC },     /* RCtrl RShift RAlt RGUI */
};

static uint8_t g_usage_of_dik[256];

static void tables_init(void)
{
    static int ready;
    if (ready) return;
    for (size_t i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
        g_usage_of_dik[k_keys[i].dik] = k_keys[i].usage;
    ready = 1;
}

static uint8_t dik_of_usage(uint32_t usage)
{
    for (size_t i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
        if (k_keys[i].usage == usage) return k_keys[i].dik;
    return 0;
}

/* ---- guest memory ----------------------------------------------------------- */

static void put32(GPTR p, uint32_t v)
{
    if (p) vt3s_store_u32(p, v);
}

static uint32_t rd32(const uint8_t *p, uint32_t off)
{
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

static void st32(uint8_t *p, uint32_t off, uint32_t v)
{
    memcpy(p + off, &v, 4);
}

static void st16(uint8_t *p, uint32_t off, uint16_t v)
{
    memcpy(p + off, &v, 2);
}

static int guid_is(GPTR p, const di_guid *want)
{
    const void *g = p ? d3d9_gmem(p, 16) : NULL;
    return g && memcmp(g, want, 16) == 0;
}

static HRESULT unsupported(const char *api)
{
    vt3s_unimpl(api);
    return DIERR_UNSUPPORTED;
}

/* ---- objects ---------------------------------------------------------------- */

typedef struct di_obj {
    uint32_t ofs, type;          /* DIOBJECTDATAFORMAT dwOfs and dwType */
} di_obj;

typedef struct di_device {
    int keyboard;                /* 1: keyboard, 0: gamepad */
    int acquired;
    int has_format;
    uint32_t data_size;          /* bytes per GetDeviceState, from SetDataFormat */
    uint32_t nobj;
    di_obj obj[DI_MAX_OBJ];
    uint32_t bufsize;            /* DIPROP_BUFFERSIZE; 0 = unbuffered */
    int32_t lo, hi;              /* DIPROP_RANGE for the axes */
    uint32_t deadzone;           /* DIPROP_DEADZONE, 1/10000 of full scale */
    uint32_t seq;                /* DIDEVICEOBJECTDATA dwSequence */
} di_device;

static int is_root(GPTR obj)
{
    return d3d9_host_of(obj, D3D9_KIND_DINPUT) != NULL;
}

static di_device *dev_of(GPTR obj)
{
    d3d9_host *h = d3d9_host_of(obj, D3D9_KIND_DIDEVICE);
    return h ? h->data : NULL;
}

static HRESULT di_qi(GPTR This, GPTR riid, GPTR ppv, uint32_t kind, const di_guid *own)
{
    put32(ppv, 0);
    if (!ppv || !riid) return DIERR_INVALIDPARAM;
    if (!d3d9_host_of(This, kind)) return DIERR_INVALIDPARAM;
    if (!guid_is(riid, &G_IUNKNOWN) && !guid_is(riid, own)) return DIERR_NOINTERFACE;
    d3d9_add_ref(This);
    put32(ppv, This);
    return DI_OK;
}

/* ---- device enumeration ----------------------------------------------------- */

/* A DIDEVICEINSTANCE for the keyboard or the gamepad, written into b (580 bytes). */
static void fill_instance(uint8_t *b, int keyboard)
{
    memset(b, 0, DIDEVICEINSTANCE_SIZE);
    st32(b, 0, DIDEVICEINSTANCE_SIZE);
    const di_guid *g = keyboard ? &G_SYSKEYBOARD : &G_JOYSTICK;
    memcpy(b + 4, g, 16);
    memcpy(b + 20, g, 16);
    st32(b, 36, keyboard ? DI8DEVTYPE_KEYBOARD : DI8DEVTYPE_GAMEPAD);
    const char *name = keyboard ? "Keyboard" : "Gamepad";
    size_t n = strlen(name);
    memcpy(b + 40, name, n);
    memcpy(b + 300, name, n);
    st16(b, 576, 1);                              /* generic desktop page */
    st16(b, 578, keyboard ? 6 : 5);               /* keyboard, gamepad */
}

/* Calls the guest enumeration callback with one device. Returns 1 to keep enumerating. */
static int call_enum(GPTR fn, GPTR pvref, int keyboard)
{
    GPTR buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DIDEVICEINSTANCE_SIZE);
    if (!buf) return 1;
    uint8_t *b = d3d9_gmem(buf, DIDEVICEINSTANCE_SIZE);
    if (b) fill_instance(b, keyboard);
    uint32_t args[2] = { buf, pvref };
    DWORD r = vt3s_call_guest_nested(fn, args, 2);
    HeapFree(GetProcessHeap(), 0, buf);
    return r != DIENUM_STOP;
}

/* ---- device state ----------------------------------------------------------- */

/* DIJOYSTATE button index -> enum vt3_button bit. */
static const uint32_t k_pad_button[11] = {
    VT3_BTN_A, VT3_BTN_B, VT3_BTN_X, VT3_BTN_Y, VT3_BTN_LB, VT3_BTN_RB,
    VT3_BTN_BACK, VT3_BTN_START, VT3_BTN_LSTICK, VT3_BTN_RSTICK, VT3_BTN_GUIDE,
};

/* D-pad bits (UP=1, DOWN=2, LEFT=4, RIGHT=8) -> POV in hundredths of a degree. */
static uint32_t pov_centidegrees(uint32_t dpad)
{
    switch (dpad & 0xFu) {
    case VT3_BTN_UP:                             return 0;
    case VT3_BTN_UP | VT3_BTN_RIGHT:             return 4500;
    case VT3_BTN_RIGHT:                          return 9000;
    case VT3_BTN_DOWN | VT3_BTN_RIGHT:           return 13500;
    case VT3_BTN_DOWN:                           return 18000;
    case VT3_BTN_DOWN | VT3_BTN_LEFT:            return 22500;
    case VT3_BTN_LEFT:                           return 27000;
    case VT3_BTN_UP | VT3_BTN_LEFT:              return 31500;
    default:                                     return 0xFFFFFFFFu;
    }
}

/* A stick value in -1..1 (or a trigger in 0..1) mapped to lo..hi. Sticks use the deadzone. */
static int32_t scale_axis(float a, int32_t lo, int32_t hi, int trigger, uint32_t deadzone)
{
    if (!trigger) {
        float mag = a < 0.0f ? -a : a;
        if (mag < (float)deadzone / 10000.0f) a = 0.0f;
    }
    float t = trigger ? a : (a + 1.0f) * 0.5f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    int64_t span = (int64_t)hi - lo;
    return (int32_t)((int64_t)lo + (int64_t)((float)span * t + 0.5f));
}

static void fill_keyboard(const di_device *d, uint8_t *buf, uint32_t cb)
{
    for (uint32_t i = 0; i < d->nobj; i++) {
        const di_obj *o = &d->obj[i];
        if (!(o->type & DIDFT_BUTTON) || o->ofs >= cb || o->ofs >= 256) continue;
        uint32_t usage = g_usage_of_dik[o->ofs];
        buf[o->ofs] = (usage && vt3_input_key_down(usage)) ? 0x80 : 0;
    }
}

static void fill_pad(const di_device *d, uint8_t *buf, uint32_t cb)
{
    const vt3_input_state *in = vt3_input_get();
    uint32_t btn = vt3_input_pad_buttons();
    int32_t centre = scale_axis(0.0f, d->lo, d->hi, 0, 0);
    int32_t axis[8];                                  /* by offset / 4: X Y Z Rx Ry Rz s0 s1 */
    axis[0] = scale_axis(in->axis[VT3_AXIS_LX], d->lo, d->hi, 0, d->deadzone);
    axis[1] = scale_axis(in->axis[VT3_AXIS_LY], d->lo, d->hi, 0, d->deadzone);
    axis[2] = centre;
    axis[3] = scale_axis(in->axis[VT3_AXIS_RX], d->lo, d->hi, 0, d->deadzone);
    axis[4] = scale_axis(in->axis[VT3_AXIS_RY], d->lo, d->hi, 0, d->deadzone);
    axis[5] = centre;
    axis[6] = scale_axis(in->axis[VT3_AXIS_LT], d->lo, d->hi, 1, 0);
    axis[7] = scale_axis(in->axis[VT3_AXIS_RT], d->lo, d->hi, 1, 0);
    uint32_t pov = pov_centidegrees(btn & 0xFu);

    for (uint32_t i = 0; i < d->nobj; i++) {
        const di_obj *o = &d->obj[i];
        if (o->type & DIDFT_AXIS) {
            if (o->ofs < 32 && o->ofs % 4 == 0 && o->ofs + 4 <= cb)
                st32(buf, o->ofs, (uint32_t)axis[o->ofs / 4]);
        } else if (o->type & DIDFT_POV) {
            if (o->ofs >= 32 && o->ofs < 48 && o->ofs % 4 == 0 && o->ofs + 4 <= cb)
                st32(buf, o->ofs, o->ofs == 32 ? pov : 0xFFFFFFFFu);
        } else if (o->type & DIDFT_BUTTON) {
            if (o->ofs >= 48 && o->ofs - 48 < 11 && o->ofs < cb)
                buf[o->ofs] = (btn & k_pad_button[o->ofs - 48]) ? 0x80 : 0;
        }
    }
}

/* ---- DirectInput8Create ----------------------------------------------------- */

HRESULT WINAPI DirectInput8Create(GPTR hinst, DWORD version, LPCGUID riid, GPTR out, LPUNKNOWN outer)
{
    put32(out, 0);
    if (!out) return DIERR_INVALIDPARAM;
    if (!guid_is(riid, &G_IDI8A)) {
        VT3_WARN("DirectInput8Create: only IDirectInput8A is implemented");
        return DIERR_NOINTERFACE;
    }
    if (outer) return DIERR_NOAGGREGATION;
    tables_init();
    GPTR obj = d3d9_obj_new(D3D9_KIND_DINPUT, NULL, 0);
    if (!obj) {
        VT3_ERROR("DirectInput8Create: no free object slot or guest heap");
        return DIERR_OUTOFMEMORY;
    }
    VT3_INFO("DirectInput8Create: IDirectInput8A at 0x%08x (version 0x%04x)", obj, version);
    put32(out, obj);
    return DI_OK;
}

/* ---- IDirectInput8A --------------------------------------------------------- */

HRESULT WINAPI IDirectInput8A_QueryInterface(GPTR This, GPTR riid, GPTR ppvObj)
{
    return di_qi(This, riid, ppvObj, D3D9_KIND_DINPUT, &G_IDI8A);
}

DWORD WINAPI IDirectInput8A_AddRef(GPTR This)
{
    return d3d9_add_ref(This);
}

DWORD WINAPI IDirectInput8A_Release(GPTR This)
{
    return d3d9_release(This);
}

HRESULT WINAPI IDirectInput8A_CreateDevice(GPTR This, GPTR rguid, GPTR lplpDirectInputDevice, GPTR pUnkOuter)
{
    put32(lplpDirectInputDevice, 0);
    if (!is_root(This) || !rguid || !lplpDirectInputDevice) return DIERR_INVALIDPARAM;
    if (pUnkOuter) return DIERR_NOAGGREGATION;
    int kb = guid_is(rguid, &G_SYSKEYBOARD);
    if (!kb && !(guid_is(rguid, &G_JOYSTICK) && vt3_input_pad_connected())) return DIERR_DEVICENOTREG;
    di_device *d = calloc(1, sizeof *d);
    if (!d) return DIERR_OUTOFMEMORY;
    d->keyboard = kb;
    d->lo = 0;
    d->hi = 65535;
    tables_init();
    GPTR obj = d3d9_obj_new(D3D9_KIND_DIDEVICE, d, This);
    if (!obj) {
        free(d);
        return DIERR_OUTOFMEMORY;
    }
    VT3_INFO("IDirectInput8A::CreateDevice: %s at 0x%08x", kb ? "keyboard" : "gamepad", obj);
    put32(lplpDirectInputDevice, obj);
    return DI_OK;
}

HRESULT WINAPI IDirectInput8A_EnumDevices(GPTR This, DWORD dwDevType, GPTR lpCallback, GPTR pvRef, DWORD dwFlags)
{
    if (!is_root(This) || !lpCallback) return DIERR_INVALIDPARAM;
    (void)dwFlags;                                    /* every device listed here is attached */
    int all = dwDevType == DI8DEVCLASS_ALL;
    int want_kb = all || dwDevType == DI8DEVCLASS_KEYBOARD || dwDevType == DI8DEVTYPE_KEYBOARD;
    int want_pad = all || dwDevType == DI8DEVCLASS_GAMECTRL || dwDevType == DI8DEVTYPE_GAMEPAD ||
                   dwDevType == DI8DEVTYPE_JOYSTICK;
    if (want_kb && !call_enum(lpCallback, pvRef, 1)) return DI_OK;
    if (want_pad && vt3_input_pad_connected()) call_enum(lpCallback, pvRef, 0);
    return DI_OK;
}

HRESULT WINAPI IDirectInput8A_GetDeviceStatus(GPTR This, GPTR rguidInstance)
{
    if (!is_root(This)) return DIERR_INVALIDPARAM;
    if (guid_is(rguidInstance, &G_SYSKEYBOARD)) return DI_OK;
    if (guid_is(rguidInstance, &G_JOYSTICK) && vt3_input_pad_connected()) return DI_OK;
    return DIERR_DEVICENOTREG;
}

HRESULT WINAPI IDirectInput8A_RunControlPanel(GPTR This, GPTR hwndOwner, DWORD dwFlags)
{
    return unsupported("IDirectInput8A::RunControlPanel");
}

HRESULT WINAPI IDirectInput8A_Initialize(GPTR This, GPTR hinst, DWORD dwVersion)
{
    if (!is_root(This)) return DIERR_INVALIDPARAM;
    return DIERR_ALREADYINITIALIZED;                  /* DirectInput8Create returns an initialized object */
}

HRESULT WINAPI IDirectInput8A_FindDevice(GPTR This, GPTR rguidClass, GPTR ptszName, GPTR pguidInstance)
{
    if (!is_root(This)) return DIERR_INVALIDPARAM;
    return DIERR_DEVICENOTREG;
}

HRESULT WINAPI IDirectInput8A_EnumDevicesBySemantics(GPTR This, GPTR ptszUserName, GPTR lpdiActionFormat,
                                                     GPTR lpCallback, GPTR pvRef, DWORD dwFlags)
{
    return unsupported("IDirectInput8A::EnumDevicesBySemantics");
}

HRESULT WINAPI IDirectInput8A_ConfigureDevices(GPTR This, GPTR lpdiCallback, GPTR lpdiCP, DWORD dwFlags,
                                               GPTR pvRefData)
{
    return unsupported("IDirectInput8A::ConfigureDevices");
}

/* ---- IDirectInputDevice8A --------------------------------------------------- */

HRESULT WINAPI IDirectInputDevice8A_QueryInterface(GPTR This, GPTR riid, GPTR ppvObj)
{
    return di_qi(This, riid, ppvObj, D3D9_KIND_DIDEVICE, &G_IDID8A);
}

DWORD WINAPI IDirectInputDevice8A_AddRef(GPTR This)
{
    return d3d9_add_ref(This);
}

DWORD WINAPI IDirectInputDevice8A_Release(GPTR This)
{
    return d3d9_release(This);
}

HRESULT WINAPI IDirectInputDevice8A_GetCapabilities(GPTR This, GPTR lpDIDevCaps)
{
    di_device *d = dev_of(This);
    const uint8_t *in = lpDIDevCaps ? d3d9_gmem(lpDIDevCaps, 4) : NULL;
    if (!d || !in) return DIERR_INVALIDPARAM;
    uint32_t size = rd32(in, 0);
    if (size < DIDEVCAPS_SIZE) return DIERR_INVALIDPARAM;
    uint8_t *out = d3d9_gmem(lpDIDevCaps, DIDEVCAPS_SIZE);
    if (!out) return DIERR_INVALIDPARAM;
    uint8_t c[DIDEVCAPS_SIZE];
    memset(c, 0, sizeof c);
    st32(c, 0, size);
    st32(c, 4, DIDC_ATTACHED | (d->keyboard ? 0u : DIDC_POLLEDDEVICE));
    st32(c, 8, d->keyboard ? DI8DEVTYPE_KEYBOARD : DI8DEVTYPE_GAMEPAD);
    st32(c, 12, d->keyboard ? 0u : 7u);              /* axes */
    st32(c, 16, d->keyboard ? 256u : 11u);           /* buttons */
    st32(c, 20, d->keyboard ? 0u : 1u);              /* POVs */
    memcpy(out, c, sizeof c);
    return DI_OK;
}

HRESULT WINAPI IDirectInputDevice8A_EnumObjects(GPTR This, GPTR lpCallback, GPTR pvRef, DWORD dwFlags)
{
    if (!dev_of(This) || !lpCallback) return DIERR_INVALIDPARAM;
    /* No objects are listed; a game that needs them will find none. */
    vt3s_unimpl("IDirectInputDevice8A::EnumObjects (no objects reported)");
    return DI_OK;
}

/* Property GUID arguments are MAKEDIPROP(n): the small integer itself. */
static uint32_t prop_id(GPTR rguidProp)
{
    return (rguidProp >= DIPROP_BUFFERSIZE && rguidProp <= DIPROP_SATURATION) ? rguidProp : 0;
}

HRESULT WINAPI IDirectInputDevice8A_GetProperty(GPTR This, GPTR rguidProp, GPTR pdiph)
{
    di_device *d = dev_of(This);
    if (!d || !pdiph) return DIERR_INVALIDPARAM;
    uint32_t id = prop_id(rguidProp);
    if (!id) return DIERR_UNSUPPORTED;
    const uint8_t *h = d3d9_gmem(pdiph, DIPROPHEADER_SIZE);
    if (!h) return DIERR_INVALIDPARAM;
    uint32_t want = id == DIPROP_RANGE ? 24u : 20u;
    if (rd32(h, 0) != want) return DIERR_INVALIDPARAM;
    uint8_t *p = d3d9_gmem(pdiph, want);
    if (!p) return DIERR_INVALIDPARAM;
    switch (id) {
    case DIPROP_BUFFERSIZE:   st32(p, 16, d->bufsize); break;
    case DIPROP_AXISMODE:     st32(p, 16, DIPROPAXISMODE_ABS); break;
    case DIPROP_GRANULARITY:  st32(p, 16, 1); break;
    case DIPROP_RANGE:        st32(p, 16, (uint32_t)d->lo); st32(p, 20, (uint32_t)d->hi); break;
    case DIPROP_DEADZONE:     st32(p, 16, d->deadzone); break;
    case DIPROP_SATURATION:   st32(p, 16, 10000); break;
    default:                  return DIERR_UNSUPPORTED;
    }
    return DI_OK;
}

HRESULT WINAPI IDirectInputDevice8A_SetProperty(GPTR This, GPTR rguidProp, GPTR pdiph)
{
    di_device *d = dev_of(This);
    if (!d || !pdiph) return DIERR_INVALIDPARAM;
    uint32_t id = prop_id(rguidProp);
    if (!id) return DIERR_UNSUPPORTED;
    const uint8_t *h = d3d9_gmem(pdiph, DIPROPHEADER_SIZE);
    if (!h) return DIERR_INVALIDPARAM;
    uint32_t want = id == DIPROP_RANGE ? 24u : 20u;
    if (rd32(h, 0) != want) return DIERR_INVALIDPARAM;
    const uint8_t *p = d3d9_gmem(pdiph, want);
    if (!p) return DIERR_INVALIDPARAM;
    uint32_t v = rd32(p, 16);
    /* Properties apply to the whole device, whatever dwHow names. */
    switch (id) {
    case DIPROP_BUFFERSIZE:
        if (v > DI_MAX_BUFSIZE) return DIERR_INVALIDPARAM;
        d->bufsize = v;
        break;
    case DIPROP_AXISMODE:
        if (v != DIPROPAXISMODE_ABS) return DIERR_UNSUPPORTED;   /* relative axes are not reported */
        break;
    case DIPROP_RANGE: {
        int32_t lo = (int32_t)v, hi = (int32_t)rd32(p, 20);
        if (lo >= hi) return DIERR_INVALIDPARAM;
        d->lo = lo;
        d->hi = hi;
        break;
    }
    case DIPROP_DEADZONE:
        if (v > 10000) return DIERR_INVALIDPARAM;
        d->deadzone = v;
        break;
    case DIPROP_SATURATION:
        break;                                       /* accepted, no effect */
    default:
        return DIERR_UNSUPPORTED;                    /* GRANULARITY is read-only */
    }
    return DI_OK;
}

HRESULT WINAPI IDirectInputDevice8A_Acquire(GPTR This)
{
    di_device *d = dev_of(This);
    if (!d) return DIERR_INVALIDPARAM;
    if (!d->keyboard && !vt3_input_pad_connected()) return DIERR_INPUTLOST;
    if (d->acquired) return DI_NOEFFECT;
    d->acquired = 1;
    VT3_INFO("IDirectInputDevice8A::Acquire: %s", d->keyboard ? "keyboard" : "gamepad");
    return DI_OK;
}

HRESULT WINAPI IDirectInputDevice8A_Unacquire(GPTR This)
{
    di_device *d = dev_of(This);
    if (!d) return DIERR_INVALIDPARAM;
    if (!d->acquired) return DI_NOEFFECT;
    d->acquired = 0;
    return DI_OK;
}

HRESULT WINAPI IDirectInputDevice8A_GetDeviceState(GPTR This, DWORD cbData, GPTR lpvData)
{
    di_device *d = dev_of(This);
    if (!d) return DIERR_INVALIDPARAM;
    if (!d->acquired) return DIERR_NOTACQUIRED;
    if (!d->has_format || cbData != d->data_size || !lpvData) return DIERR_INVALIDPARAM;
    if (!d->keyboard && !vt3_input_pad_connected()) return DIERR_INPUTLOST;
    uint8_t *buf = d3d9_gmem(lpvData, cbData);
    if (!buf) return DIERR_INVALIDPARAM;
    memset(buf, 0, cbData);
    if (d->keyboard) fill_keyboard(d, buf, cbData);
    else fill_pad(d, buf, cbData);
    static int said_key;                 /* first key-down seen by the game, for diagnosis */
    if (!said_key && d->keyboard) {
        for (uint32_t i = 0; i < cbData; i++) {
            if (buf[i]) {
                said_key = 1;
                VT3_INFO("IDirectInputDevice8A::GetDeviceState: key 0x%02x down", (unsigned)i);
                break;
            }
        }
    }
    return DI_OK;
}

HRESULT WINAPI IDirectInputDevice8A_GetDeviceData(GPTR This, DWORD cbObjectData, GPTR rgdod, GPTR pdwInOut,
                                                  DWORD dwFlags)
{
    di_device *d = dev_of(This);
    uint8_t *cnt = pdwInOut ? d3d9_gmem(pdwInOut, 4) : NULL;
    if (!d || !cnt) return DIERR_INVALIDPARAM;
    if (!d->acquired) return DIERR_NOTACQUIRED;
    if (d->bufsize == 0) return DIERR_NOTBUFFERED;
    if (cbObjectData != DIDEVICEOBJECTDATA_SIZE) return DIERR_INVALIDPARAM;
    uint32_t max = rd32(cnt, 0);
    uint32_t got = 0;
    if (d->keyboard && max && rgdod) {
        uint32_t cap = max < 64u ? max : 64u;
        if (cap > d->bufsize) cap = d->bufsize;
        vt3_key_event ev[64];
        uint32_t n = vt3_input_take_keys(ev, cap, (dwFlags & DIGDD_PEEK) != 0);
        uint8_t *out = d3d9_gmem(rgdod, cap * DIDEVICEOBJECTDATA_SIZE);
        for (uint32_t i = 0; out && i < n; i++) {
            uint8_t dik = dik_of_usage(ev[i].scancode);
            if (!dik) continue;
            uint8_t *o = out + got * DIDEVICEOBJECTDATA_SIZE;
            st32(o, 0, dik);
            st32(o, 4, ev[i].down ? 0x80u : 0u);
            st32(o, 8, ev[i].ms);
            st32(o, 12, ++d->seq);
            got++;
        }
    }
    st32(cnt, 0, got);
    return DI_OK;
}

HRESULT WINAPI IDirectInputDevice8A_SetDataFormat(GPTR This, GPTR lpdf)
{
    di_device *d = dev_of(This);
    if (!d) return DIERR_INVALIDPARAM;
    if (d->acquired) return DIERR_ACQUIRED;
    const uint8_t *f = d3d9_gmem(lpdf, DIDATAFORMAT_SIZE);
    if (!f) return DIERR_INVALIDPARAM;
    uint32_t objsz = rd32(f, 4), data_size = rd32(f, 12), nobj = rd32(f, 16);
    GPTR rgodf = rd32(f, 20);
    if (objsz != DIOBJECTDATAFORMAT_SIZE || data_size == 0 || data_size > 0x10000u || nobj > DI_MAX_OBJ)
        return DIERR_INVALIDPARAM;
    const uint8_t *o = NULL;
    if (nobj) {
        o = d3d9_gmem(rgodf, nobj * DIOBJECTDATAFORMAT_SIZE);
        if (!o) return DIERR_INVALIDPARAM;
    }
    for (uint32_t i = 0; i < nobj; i++) {
        d->obj[i].ofs = rd32(o, i * DIOBJECTDATAFORMAT_SIZE + 4);
        d->obj[i].type = rd32(o, i * DIOBJECTDATAFORMAT_SIZE + 8);
        if (d->obj[i].ofs >= data_size) {
            d->nobj = 0;
            d->has_format = 0;
            return DIERR_INVALIDPARAM;
        }
    }
    d->nobj = nobj;
    d->data_size = data_size;
    d->has_format = 1;
    VT3_INFO("IDirectInputDevice8A::SetDataFormat: %s, %u objects, %u bytes",
             d->keyboard ? "keyboard" : "gamepad", (unsigned)nobj, (unsigned)data_size);
    return DI_OK;
}

HRESULT WINAPI IDirectInputDevice8A_SetEventNotification(GPTR This, GPTR hEvent)
{
    if (!dev_of(This)) return DIERR_INVALIDPARAM;
    return unsupported("IDirectInputDevice8A::SetEventNotification");
}

HRESULT WINAPI IDirectInputDevice8A_SetCooperativeLevel(GPTR This, GPTR hwnd, DWORD dwFlags)
{
    if (!dev_of(This)) return DIERR_INVALIDPARAM;
    return DI_OK;                                    /* one application, one window: no arbitration */
}

HRESULT WINAPI IDirectInputDevice8A_GetObjectInfo(GPTR This, GPTR pdidoi, DWORD dwObj, DWORD dwHow)
{
    if (!dev_of(This)) return DIERR_INVALIDPARAM;
    return DIERR_NOTFOUND;                           /* no objects are listed (see EnumObjects) */
}

HRESULT WINAPI IDirectInputDevice8A_GetDeviceInfo(GPTR This, GPTR pdidi)
{
    di_device *d = dev_of(This);
    const uint8_t *in = pdidi ? d3d9_gmem(pdidi, 4) : NULL;
    if (!d || !in) return DIERR_INVALIDPARAM;
    uint32_t size = rd32(in, 0);
    uint8_t *out = d3d9_gmem(pdidi, size);
    if (!out || size < 4) return DIERR_INVALIDPARAM;
    uint8_t full[DIDEVICEINSTANCE_SIZE];
    fill_instance(full, d->keyboard);
    memcpy(out, full, size < DIDEVICEINSTANCE_SIZE ? size : DIDEVICEINSTANCE_SIZE);
    st32(out, 0, size);                              /* dwSize is the caller's */
    return DI_OK;
}

HRESULT WINAPI IDirectInputDevice8A_RunControlPanel(GPTR This, GPTR hwndOwner, DWORD dwFlags)
{
    return unsupported("IDirectInputDevice8A::RunControlPanel");
}

HRESULT WINAPI IDirectInputDevice8A_Initialize(GPTR This, GPTR hinst, DWORD dwVersion, GPTR rguid)
{
    if (!dev_of(This)) return DIERR_INVALIDPARAM;
    return DIERR_ALREADYINITIALIZED;                 /* CreateDevice returns an initialized device */
}

HRESULT WINAPI IDirectInputDevice8A_CreateEffect(GPTR This, GPTR rguid, GPTR lpeff, GPTR ppdeff, GPTR pUnkOuter)
{
    put32(ppdeff, 0);
    return unsupported("IDirectInputDevice8A::CreateEffect (force feedback)");
}

HRESULT WINAPI IDirectInputDevice8A_EnumEffects(GPTR This, GPTR lpCallback, GPTR pvRef, DWORD dwEffType)
{
    return unsupported("IDirectInputDevice8A::EnumEffects");
}

HRESULT WINAPI IDirectInputDevice8A_GetEffectInfo(GPTR This, GPTR pdei, GPTR rguid)
{
    return unsupported("IDirectInputDevice8A::GetEffectInfo");
}

HRESULT WINAPI IDirectInputDevice8A_GetForceFeedbackState(GPTR This, GPTR pdwOut)
{
    return unsupported("IDirectInputDevice8A::GetForceFeedbackState");
}

HRESULT WINAPI IDirectInputDevice8A_SendForceFeedbackCommand(GPTR This, DWORD dwFlags)
{
    return unsupported("IDirectInputDevice8A::SendForceFeedbackCommand");
}

HRESULT WINAPI IDirectInputDevice8A_EnumCreatedEffectObjects(GPTR This, GPTR lpCallback, GPTR pvRef, DWORD fl)
{
    return unsupported("IDirectInputDevice8A::EnumCreatedEffectObjects");
}

HRESULT WINAPI IDirectInputDevice8A_Escape(GPTR This, GPTR lpEscape)
{
    return unsupported("IDirectInputDevice8A::Escape");
}

HRESULT WINAPI IDirectInputDevice8A_Poll(GPTR This)
{
    di_device *d = dev_of(This);
    if (!d) return DIERR_INVALIDPARAM;
    if (!d->acquired) return DIERR_NOTACQUIRED;
    return d->keyboard ? DI_NOEFFECT : DI_OK;        /* the keyboard is not a polled device */
}

HRESULT WINAPI IDirectInputDevice8A_SendDeviceData(GPTR This, DWORD cbObjectData, GPTR rgdod, GPTR pdwInOut,
                                                   DWORD fl)
{
    return unsupported("IDirectInputDevice8A::SendDeviceData");
}

HRESULT WINAPI IDirectInputDevice8A_EnumEffectsInFile(GPTR This, GPTR lpszFileName, GPTR pec, GPTR pvRef,
                                                      DWORD dwFlags)
{
    return unsupported("IDirectInputDevice8A::EnumEffectsInFile");
}

HRESULT WINAPI IDirectInputDevice8A_WriteEffectToFile(GPTR This, GPTR lpszFileName, DWORD dwEntries,
                                                      GPTR rgDiFileEft, DWORD dwFlags)
{
    return unsupported("IDirectInputDevice8A::WriteEffectToFile");
}

HRESULT WINAPI IDirectInputDevice8A_BuildActionMap(GPTR This, GPTR lpdiaf, GPTR lpszUserName, DWORD dwFlags)
{
    return unsupported("IDirectInputDevice8A::BuildActionMap");
}

HRESULT WINAPI IDirectInputDevice8A_SetActionMap(GPTR This, GPTR lpdiaf, GPTR lpszUserName, DWORD dwFlags)
{
    return unsupported("IDirectInputDevice8A::SetActionMap");
}

HRESULT WINAPI IDirectInputDevice8A_GetImageInfo(GPTR This, GPTR lpdiDevImageInfoHeader)
{
    return unsupported("IDirectInputDevice8A::GetImageInfo");
}

/* ---- vtable names and object hooks (called from d3d9_object.c) -------------- */

static const char *const k_names_root[] = {
    "IDirectInput8A_QueryInterface",
    "IDirectInput8A_AddRef",
    "IDirectInput8A_Release",
    "IDirectInput8A_CreateDevice",
    "IDirectInput8A_EnumDevices",
    "IDirectInput8A_GetDeviceStatus",
    "IDirectInput8A_RunControlPanel",
    "IDirectInput8A_Initialize",
    "IDirectInput8A_FindDevice",
    "IDirectInput8A_EnumDevicesBySemantics",
    "IDirectInput8A_ConfigureDevices",
};

static const char *const k_names_dev[] = {
    "IDirectInputDevice8A_QueryInterface",
    "IDirectInputDevice8A_AddRef",
    "IDirectInputDevice8A_Release",
    "IDirectInputDevice8A_GetCapabilities",
    "IDirectInputDevice8A_EnumObjects",
    "IDirectInputDevice8A_GetProperty",
    "IDirectInputDevice8A_SetProperty",
    "IDirectInputDevice8A_Acquire",
    "IDirectInputDevice8A_Unacquire",
    "IDirectInputDevice8A_GetDeviceState",
    "IDirectInputDevice8A_GetDeviceData",
    "IDirectInputDevice8A_SetDataFormat",
    "IDirectInputDevice8A_SetEventNotification",
    "IDirectInputDevice8A_SetCooperativeLevel",
    "IDirectInputDevice8A_GetObjectInfo",
    "IDirectInputDevice8A_GetDeviceInfo",
    "IDirectInputDevice8A_RunControlPanel",
    "IDirectInputDevice8A_Initialize",
    "IDirectInputDevice8A_CreateEffect",
    "IDirectInputDevice8A_EnumEffects",
    "IDirectInputDevice8A_GetEffectInfo",
    "IDirectInputDevice8A_GetForceFeedbackState",
    "IDirectInputDevice8A_SendForceFeedbackCommand",
    "IDirectInputDevice8A_EnumCreatedEffectObjects",
    "IDirectInputDevice8A_Escape",
    "IDirectInputDevice8A_Poll",
    "IDirectInputDevice8A_SendDeviceData",
    "IDirectInputDevice8A_EnumEffectsInFile",
    "IDirectInputDevice8A_WriteEffectToFile",
    "IDirectInputDevice8A_BuildActionMap",
    "IDirectInputDevice8A_SetActionMap",
    "IDirectInputDevice8A_GetImageInfo",
};

_Static_assert(sizeof k_names_root / sizeof k_names_root[0] == sizeof(IDirectInput8AVtbl) / sizeof(GPTR),
               "IDirectInput8A vtable: one name per slot");
_Static_assert(sizeof k_names_dev / sizeof k_names_dev[0] == sizeof(IDirectInputDevice8AVtbl) / sizeof(GPTR),
               "IDirectInputDevice8A vtable: one name per slot");

const char *const *dinput_vtbl_names(uint32_t kind, size_t *count)
{
    switch (kind) {
    case D3D9_KIND_DINPUT:
        *count = sizeof k_names_root / sizeof k_names_root[0];
        return k_names_root;
    case D3D9_KIND_DIDEVICE:
        *count = sizeof k_names_dev / sizeof k_names_dev[0];
        return k_names_dev;
    default:
        return NULL;
    }
}

void dinput_obj_free(uint32_t kind, void *data)
{
    (void)kind;
    free(data);                                      /* the root object has no host data */
}
