/*
 * DirectSound. IDirectSound and IDirectSoundBuffer are guest COM objects (D3D9_KIND_DSOUND,
 * D3D9_KIND_DSBUFFER; vtables from the generated d3d9_com.h). A buffer keeps its PCM in guest
 * heap memory, Lock hands the game real storage, and the play cursor follows the wall clock,
 * so a game that streams audio paces itself as it would with a sound card.
 *
 * The mixer thread renders every playing buffer to the output device through the audio layer
 * (vt3_audio.h). For each output frame it reads the buffer at the position the wall clock
 * gives, so it reads only what the game has already written: the game writes about 50 ms
 * ahead of that position. The output ring adds about 40 ms of delay before the sound is heard,
 * which does not change what is read. Buffers are converted to 48 kHz stereo 16-bit with
 * nearest-sample resampling and a surround downmix. If no device opens, playback stays silent
 * and the game still runs.
 *
 * A failed creation still writes a stub object (no host data, methods report
 * DSERR_UNSUPPORTED), so a guest creation call does not leave its output empty.
 *
 * Guest layouts (SDK; offsets checked against the mingw-w64 14.0.0 headers):
 *   WAVEFORMATEX  18 bytes, packed.
 *   DSBUFFERDESC  dwSize@0 dwFlags@4 dwBufferBytes@8 dwReserved@12 lpwfxFormat@16.
 *   DSCAPS        92 bytes: dwSize@0 dwFlags@4 dwMinSecondarySampleRate@8
 *                 dwMaxSecondarySampleRate@12 dwPrimaryBuffers@16.
 *   DSBCAPS       20 bytes: dwSize@0 dwFlags@4 dwBufferBytes@8.
 */
#include "d3d9_internal.h"
#include "vt3/vt3_audio.h"
#include "vt3/vt3_log.h"
#include "win32/dsound.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WFX_BYTES 18u
#define DSBDESC_BYTES 20u
#define DSCAPS_BYTES 92u
#define DSBCAPS_BYTES 20u

typedef struct ds_device {
    uint32_t coop_level;
} ds_device;

typedef struct ds_buffer {
    GPTR storage;                /* guest heap: bytes of PCM */
    uint32_t bytes;
    uint8_t fmt[WFX_BYTES];      /* WAVEFORMATEX as the game gave it */
    uint32_t pos;                /* play cursor while stopped, in bytes */
    uint32_t play_base;          /* play cursor when the current playback started */
    uint64_t play_ns;            /* monotonic time when the current playback started */
    int playing;
    int looping;
    int32_t volume, pan;
    uint32_t freq;
} ds_buffer;

/* ---- output mixer ----------------------------------------------------------
 * g_mu guards the list of playing buffers and every buffer's play state while it is
 * in the list. Lock/Unlock do not take it: the game writes storage the mixer reads, which
 * is the same race a real sound card has with the game's write cursor. */
#define MIX_RATE 48000u
#define MIX_CH 2u
#define MIX_RING_MS 40u
#define MIX_MAX_FRAMES 960u
#define MIX_MAX_ACTIVE 64

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static ds_buffer *g_active[MIX_MAX_ACTIVE];
static int g_nactive;
static vt3_audio *g_out;
static uint64_t g_mix_next_ns;

/* ---- guest memory and time ------------------------------------------------ */

static uint32_t rd32(const uint8_t *p, uint32_t off)
{
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

static void set32(uint8_t *p, uint32_t off, uint32_t v)
{
    memcpy(p + off, &v, 4);
}

static void put32(GPTR p, uint32_t v)
{
    if (p) vt3s_store_u32(p, v);
}

static void put_bytes(GPTR p, const void *src, uint32_t n)
{
    void *q = d3d9_gmem(p, n);
    if (q) memcpy(q, src, n);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* ---- playback model -------------------------------------------------------- */

static uint32_t avg_bytes_per_sec(const ds_buffer *b)
{
    return rd32(b->fmt, 8);
}

/* The write cursor runs this far ahead of the play cursor: 50 ms, or half the buffer. */
static uint32_t write_lead(const ds_buffer *b)
{
    uint32_t lead = avg_bytes_per_sec(b) / 20u;
    return lead < b->bytes / 2u ? lead : b->bytes / 2u;
}

static uint32_t play_cursor(const ds_buffer *b)
{
    if (!b->playing || b->bytes == 0) return b->pos;
    uint64_t us = (now_ns() - b->play_ns) / 1000u;
    uint64_t moved = us * avg_bytes_per_sec(b) / 1000000u;
    return (uint32_t)((b->play_base + moved) % b->bytes);
}

static uint32_t write_cursor(const ds_buffer *b)
{
    return b->bytes ? (play_cursor(b) + write_lead(b)) % b->bytes : 0;
}

/* ---- the mixer ------------------------------------------------------------- */

static void active_add_locked(ds_buffer *b)
{
    for (int i = 0; i < g_nactive; i++) {
        if (g_active[i] == b) return;
    }
    if (g_nactive < MIX_MAX_ACTIVE) g_active[g_nactive++] = b;
}

static void active_remove_locked(ds_buffer *b)
{
    for (int i = 0; i < g_nactive; i++) {
        if (g_active[i] == b) {
            g_active[i] = g_active[--g_nactive];
            return;
        }
    }
}

static uint16_t fmt16(const uint8_t *f, uint32_t off)
{
    return (uint16_t)(f[off] | (f[off + 1] << 8));
}

/* VT3_TRACE_AUDIO=1 logs buffer creation, Play and Stop. */
static int dsound_trace(void)
{
    static int on = -1;
    if (on < 0) on = getenv("VT3_TRACE_AUDIO") != NULL;
    return on;
}

/* A signed 16-bit value from one 8- or 16-bit PCM sample. */
static int32_t pcm_sample(const uint8_t *p, uint32_t bits)
{
    return bits == 16 ? (int32_t)(int16_t)(p[0] | (p[1] << 8)) : ((int32_t)p[0] - 128) * 256;
}

/* Adds the sound of one playing buffer to acc for frames output frames. Output frame j is at
 * time t0 + j, so the source position comes from the same wall-clock model as GetCurrentPosition. */
static void mix_buffer_locked(ds_buffer *b, int32_t *acc, uint32_t frames, uint64_t t0)
{
    uint32_t nch = fmt16(b->fmt, 2), bits = fmt16(b->fmt, 14), align = fmt16(b->fmt, 12);
    uint32_t rate = rd32(b->fmt, 4);
    if (nch < 1 || nch > 8 || (bits != 8 && bits != 16) || align == 0 || rate == 0 || b->bytes < align) return;
    const uint8_t *data = d3d9_gmem(b->storage, b->bytes);
    if (!data) return;
    uint32_t src_frames = b->bytes / align;
    uint32_t bps = avg_bytes_per_sec(b);
    double gain = b->volume <= -10000 ? 0.0 : pow(10.0, b->volume / 2000.0);   /* hundredths of dB */
    uint32_t bytes_per_sample = bits / 8;
    for (uint32_t j = 0; j < frames; j++) {
        uint64_t t = t0 + (uint64_t)j * 1000000000u / MIX_RATE;
        double moved = t > b->play_ns ? (double)(t - b->play_ns) * bps / 1e9 : 0.0;
        double pos = (double)b->play_base + moved;
        if (!b->looping && pos >= (double)b->bytes) {
            /* A non-looping buffer stops at its end. */
            b->playing = 0;
            b->pos = 0;
            active_remove_locked(b);
            return;
        }
        uint64_t f = ((uint64_t)pos / align) % src_frames;
        const uint8_t *fr = data + f * align;
        int32_t l, r;
        if (nch == 1) {
            l = r = pcm_sample(fr, bits);
        } else if (nch == 2) {
            l = pcm_sample(fr, bits);
            r = pcm_sample(fr + bytes_per_sample, bits);
        } else {
            /* Surround in SPEAKER order: FL, FR, FC, LFE, BL, BR (quad: FL, FR, BL, BR). The centre and
             * the rears fold into the stereo pair at -3 dB; the LFE channel is left out. */
            int32_t fl = pcm_sample(fr, bits), fr_ = pcm_sample(fr + bytes_per_sample, bits);
            int32_t c = nch == 4 ? 0 : pcm_sample(fr + 2 * bytes_per_sample, bits);
            int32_t bl = nch == 4 ? pcm_sample(fr + 2 * bytes_per_sample, bits)
                       : nch >= 6 ? pcm_sample(fr + 4 * bytes_per_sample, bits) : 0;
            int32_t br = nch == 4 ? pcm_sample(fr + 3 * bytes_per_sample, bits)
                       : nch >= 6 ? pcm_sample(fr + 5 * bytes_per_sample, bits) : 0;
            l = fl + (int32_t)(0.7071 * (c + bl));
            r = fr_ + (int32_t)(0.7071 * (c + br));
        }
        acc[2 * j] += (int32_t)(l * gain);
        acc[2 * j + 1] += (int32_t)(r * gain);
    }
}

static void *mix_thread(void *arg)
{
    (void)arg;
    static int32_t acc[MIX_MAX_FRAMES * MIX_CH];
    static int16_t out[MIX_MAX_FRAMES * MIX_CH];
    /* VT3_TRACE_AUDIO=1 logs once a second: buffers playing, peak level, device underruns. */
    const int trace = getenv("VT3_TRACE_AUDIO") != NULL;
    uint64_t window_ns = now_ns();
    int32_t window_peak = 0;
    for (;;) {
        struct timespec ts = { 0, 4000000 };          /* 4 ms */
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&g_mu);
        uint64_t now = now_ns();
        if (now > g_mix_next_ns + 50000000u) g_mix_next_ns = now - 10000000u;   /* 50 ms behind: resync */
        uint64_t due = now > g_mix_next_ns ? (now - g_mix_next_ns) * MIX_RATE / 1000000000u : 0;
        uint64_t space = vt3_audio_space(g_out) / (MIX_CH * sizeof(int16_t));
        uint64_t frames = due < space ? due : space;
        if (frames > MIX_MAX_FRAMES) frames = MIX_MAX_FRAMES;
        if (frames > 0) {
            memset(acc, 0, (size_t)frames * MIX_CH * sizeof acc[0]);
            /* Downward, so a buffer that stops and is removed does not skip the one moved into its slot. */
            for (int i = g_nactive - 1; i >= 0; i--) {
                if (i < g_nactive) mix_buffer_locked(g_active[i], acc, (uint32_t)frames, g_mix_next_ns);
            }
            int32_t peak = 0;
            for (size_t k = 0; k < (size_t)frames * MIX_CH; k++) {
                int32_t v = acc[k];
                out[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                if (v > peak) peak = v;
                if (-v > peak) peak = -v;
            }
            vt3_audio_write(g_out, out, (size_t)frames * MIX_CH * sizeof(int16_t));
            g_mix_next_ns += frames * 1000000000u / MIX_RATE;
            if (trace) {
                window_peak = peak > window_peak ? peak : window_peak;
                if (now - window_ns >= 1000000000u) {
                    VT3_INFO("audio: %d playing, peak %d in the last second, underruns %u", g_nactive,
                             (int)window_peak, (unsigned)vt3_audio_underruns(g_out));
                    window_ns = now;
                    window_peak = 0;
                }
            }
        }
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

/* Opens the output and starts the mixer, once. Without a device the buffers stay silent. */
static void mixer_start_once(void)
{
    static int started;
    if (started) return;
    started = 1;
    vt3_audio_format fmt = { MIX_RATE, (int)MIX_CH };
    size_t ring = (size_t)MIX_RATE * MIX_CH * sizeof(int16_t) * MIX_RING_MS / 1000u;
    if (vt3_audio_open(&g_out, fmt, ring) != 0) {
        g_out = NULL;
        VT3_WARN("DirectSound: no audio device; playback stays silent");
        return;
    }
    g_mix_next_ns = now_ns();
    pthread_t th;
    if (pthread_create(&th, NULL, mix_thread, NULL) != 0) {
        VT3_WARN("DirectSound: could not start the mixer; playback stays silent");
        return;
    }
    pthread_detach(th);
    vt3_audio_set_playing(g_out, 1);
    VT3_INFO("DirectSound: mixing to a %u Hz stereo output", (unsigned)MIX_RATE);
}

/* ---- objects --------------------------------------------------------------- */

static ds_buffer *buf_of(GPTR obj)
{
    d3d9_host *h = d3d9_host_of(obj, D3D9_KIND_DSBUFFER);
    return h ? h->data : NULL;
}

static ds_device *dev_of(GPTR obj)
{
    d3d9_host *h = d3d9_host_of(obj, D3D9_KIND_DSOUND);
    return h ? h->data : NULL;
}

/* A buffer with its own storage, or 0. fmt is the 18-byte WAVEFORMATEX from the guest. */
static GPTR buffer_new(GPTR parent, uint32_t bytes, const uint8_t *fmt)
{
    ds_buffer *b = calloc(1, sizeof *b);
    if (!b) return 0;
    b->storage = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
    if (!b->storage) {
        free(b);
        return 0;
    }
    b->bytes = bytes;
    memcpy(b->fmt, fmt, WFX_BYTES);
    b->freq = rd32(fmt, 4);
    b->volume = DSBVOLUME_MAX;
    GPTR obj = d3d9_obj_new(D3D9_KIND_DSBUFFER, b, parent);
    if (!obj) {
        HeapFree(GetProcessHeap(), 0, b->storage);
        free(b);
    }
    return obj;
}

void dsound_free(uint32_t kind, void *data)
{
    if (kind == D3D9_KIND_DSBUFFER) {
        ds_buffer *b = data;
        pthread_mutex_lock(&g_mu);
        active_remove_locked(b);
        pthread_mutex_unlock(&g_mu);
        if (b->storage) HeapFree(GetProcessHeap(), 0, b->storage);
    }
    free(data);
}

/* A primary buffer has no size of its own: it is the mix target. */
#define DS_PRIMARY_BYTES 65536u

/* DirectSound's default primary format: PCM, 22.05 kHz, 8-bit, mono. */
static void default_primary_fmt(uint8_t f[WFX_BYTES])
{
    uint16_t tag = 1, ch = 1, align = 1, bits = 8;
    uint32_t rate = 22050, avg = 22050, cb = 0;
    memset(f, 0, WFX_BYTES);
    memcpy(f + 0, &tag, 2);
    memcpy(f + 2, &ch, 2);
    memcpy(f + 4, &rate, 4);
    memcpy(f + 8, &avg, 4);
    memcpy(f + 12, &align, 2);
    memcpy(f + 14, &bits, 2);
    memcpy(f + 16, &cb, 2);
}

/* ---- DirectSoundCreate ------------------------------------------------------ */

HRESULT WINAPI DirectSoundCreate(LPCGUID device, GPTR out_ds, LPUNKNOWN outer)
{
    (void)device;                /* there is one output, so the device GUID is not used */
    if (!out_ds) return DSERR_INVALIDPARAM;
    put32(out_ds, 0);
    if (outer) return DSERR_NOAGGREGATION;
    ds_device *d = calloc(1, sizeof *d);
    if (!d) return E_OUTOFMEMORY_D3D9;
    GPTR obj = d3d9_obj_new(D3D9_KIND_DSOUND, d, 0);
    if (!obj) {
        free(d);
        VT3_ERROR("DirectSoundCreate: no free object slot or guest heap");
        return E_OUTOFMEMORY_D3D9;
    }
    mixer_start_once();
    VT3_INFO("DirectSoundCreate: IDirectSound at 0x%08x", obj);
    put32(out_ds, obj);
    return DS_OK;
}

/* ---- IDirectSound ---------------------------------------------------------- */

HRESULT WINAPI IDirectSound_CreateSoundBuffer(GPTR This, GPTR pcDSBufferDesc, GPTR ppDSBuffer, GPTR pUnkOuter)
{
    put32(ppDSBuffer, 0);
    if (!dev_of(This) || !ppDSBuffer || !pcDSBufferDesc) return DSERR_INVALIDPARAM;
    if (pUnkOuter) return DSERR_NOAGGREGATION;
    const uint8_t *desc = d3d9_gmem(pcDSBufferDesc, DSBDESC_BYTES);
    if (!desc) return DSERR_INVALIDPARAM;
    uint32_t flags = rd32(desc, 4);
    uint32_t bytes = rd32(desc, 8);
    GPTR wfx = rd32(desc, 16);
    uint8_t dflt[WFX_BYTES];
    const uint8_t *fmt = wfx ? d3d9_gmem(wfx, WFX_BYTES) : NULL;
    if (flags & DSBCAPS_PRIMARYBUFFER) {
        if (bytes == 0) bytes = DS_PRIMARY_BYTES;
        if (!fmt) {
            default_primary_fmt(dflt);
            fmt = dflt;
        }
    }
    if (bytes == 0 || !fmt) {
        VT3_WARN("IDirectSound::CreateSoundBuffer: bad description (%u bytes), returning a stub", bytes);
        put32(ppDSBuffer, d3d9_new_stub(D3D9_KIND_DSBUFFER, This));
        return DSERR_INVALIDPARAM;
    }
    GPTR obj = buffer_new(This, bytes, fmt);
    if (dsound_trace())
        VT3_INFO("DirectSound: CreateSoundBuffer flags 0x%x, %u bytes, %u Hz, %u ch, %u bit -> 0x%08x",
                 (unsigned)flags, (unsigned)bytes, (unsigned)rd32(fmt, 4), (unsigned)fmt16(fmt, 2),
                 (unsigned)fmt16(fmt, 14), (unsigned)obj);
    if (!obj) {
        VT3_WARN("IDirectSound::CreateSoundBuffer: no memory for %u bytes, returning a stub", bytes);
        put32(ppDSBuffer, d3d9_new_stub(D3D9_KIND_DSBUFFER, This));
        return E_OUTOFMEMORY_D3D9;
    }
    put32(ppDSBuffer, obj);
    return DS_OK;
}

HRESULT WINAPI IDirectSound_GetCaps(GPTR This, GPTR pDSCaps)
{
    if (!dev_of(This) || !pDSCaps) return DSERR_INVALIDPARAM;
    const uint8_t *in = d3d9_gmem(pDSCaps, 4);
    if (!in) return DSERR_INVALIDPARAM;
    uint32_t size = rd32(in, 0);
    if (size < 4) return DSERR_INVALIDPARAM;
    uint8_t caps[DSCAPS_BYTES];
    memset(caps, 0, sizeof caps);
    memcpy(caps, in, 4);                          /* dwSize is the caller's */
    set32(caps, 4, DSCAPS_EMULDRIVER);
    set32(caps, 8, DSBFREQUENCY_MIN);
    set32(caps, 12, DSBFREQUENCY_MAX);
    set32(caps, 16, 1);                           /* one primary buffer */
    put_bytes(pDSCaps, caps, size < DSCAPS_BYTES ? size : DSCAPS_BYTES);
    return DS_OK;
}

HRESULT WINAPI IDirectSound_DuplicateSoundBuffer(GPTR This, GPTR pdsbOriginal, GPTR ppdsbDuplicate)
{
    put32(ppdsbDuplicate, 0);
    if (!dev_of(This) || !ppdsbDuplicate) return DSERR_INVALIDPARAM;
    ds_buffer *src = buf_of(pdsbOriginal);
    if (!src) {
        put32(ppdsbDuplicate, d3d9_new_stub(D3D9_KIND_DSBUFFER, This));
        return DSERR_INVALIDPARAM;
    }
    GPTR obj = buffer_new(This, src->bytes, src->fmt);
    if (!obj) {
        put32(ppdsbDuplicate, d3d9_new_stub(D3D9_KIND_DSBUFFER, This));
        return E_OUTOFMEMORY_D3D9;
    }
    const void *from = d3d9_gmem(src->storage, src->bytes);
    void *to = d3d9_gmem(buf_of(obj)->storage, src->bytes);
    if (from && to) memcpy(to, from, src->bytes);
    put32(ppdsbDuplicate, obj);
    return DS_OK;
}

HRESULT WINAPI IDirectSound_SetCooperativeLevel(GPTR This, GPTR hwnd, DWORD dwLevel)
{
    (void)hwnd;
    ds_device *d = dev_of(This);
    if (!d) return DSERR_INVALIDPARAM;
    d->coop_level = dwLevel;
    return DS_OK;
}

HRESULT WINAPI IDirectSound_Compact(GPTR This)
{
    return dev_of(This) ? DS_OK : DSERR_INVALIDPARAM;
}

HRESULT WINAPI IDirectSound_GetSpeakerConfig(GPTR This, GPTR pdwSpeakerConfig)
{
    if (!dev_of(This) || !pdwSpeakerConfig) return DSERR_INVALIDPARAM;
    put32(pdwSpeakerConfig, DSSPEAKER_STEREO);
    return DS_OK;
}

HRESULT WINAPI IDirectSound_SetSpeakerConfig(GPTR This, DWORD dwSpeakerConfig)
{
    (void)dwSpeakerConfig;
    return dev_of(This) ? DS_OK : DSERR_INVALIDPARAM;
}

HRESULT WINAPI IDirectSound_Initialize(GPTR This, GPTR pcGuidDevice)
{
    (void)pcGuidDevice;
    if (!dev_of(This)) return DSERR_UNSUPPORTED;
    return DSERR_ALREADYINITIALIZED;              /* DirectSoundCreate returns an initialized object */
}

/* ---- IDirectSoundBuffer ---------------------------------------------------- */

HRESULT WINAPI IDirectSoundBuffer_GetCaps(GPTR This, GPTR pDSBufferCaps)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (!pDSBufferCaps) return DSERR_INVALIDPARAM;
    const uint8_t *in = d3d9_gmem(pDSBufferCaps, 4);
    if (!in) return DSERR_INVALIDPARAM;
    uint32_t size = rd32(in, 0);
    if (size < 4) return DSERR_INVALIDPARAM;
    uint8_t caps[DSBCAPS_BYTES];
    memset(caps, 0, sizeof caps);
    memcpy(caps, in, 4);
    set32(caps, 4, DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLDEFAULT);
    set32(caps, 8, b->bytes);
    put_bytes(pDSBufferCaps, caps, size < DSBCAPS_BYTES ? size : DSBCAPS_BYTES);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_GetCurrentPosition(GPTR This, GPTR pdwCurrentPlayCursor, GPTR pdwCurrentWriteCursor)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    put32(pdwCurrentPlayCursor, play_cursor(b));
    put32(pdwCurrentWriteCursor, write_cursor(b));
    if (dsound_trace())
        VT3_INFO("DirectSound: GetCurrentPosition 0x%08x play %u write %u (playing %d) t=%llu ms thr %llu", (unsigned)This,
                 (unsigned)play_cursor(b), (unsigned)write_cursor(b), b->playing,
                 (unsigned long long)(now_ns() / 1000000u % 100000u), (unsigned long long)((uintptr_t)pthread_self() % 1000u));
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_GetFormat(GPTR This, GPTR pwfxFormat, DWORD dwSizeAllocated, GPTR pdwSizeWritten)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (!pwfxFormat && !pdwSizeWritten) return DSERR_INVALIDPARAM;
    if (pwfxFormat) {
        if (dwSizeAllocated < WFX_BYTES) return DSERR_INVALIDPARAM;
        put_bytes(pwfxFormat, b->fmt, WFX_BYTES);
    }
    put32(pdwSizeWritten, WFX_BYTES);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_GetVolume(GPTR This, GPTR plVolume)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (!plVolume) return DSERR_INVALIDPARAM;
    put32(plVolume, (uint32_t)b->volume);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_GetPan(GPTR This, GPTR plPan)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (!plPan) return DSERR_INVALIDPARAM;
    put32(plPan, (uint32_t)b->pan);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_GetFrequency(GPTR This, GPTR pdwFrequency)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (!pdwFrequency) return DSERR_INVALIDPARAM;
    put32(pdwFrequency, b->freq);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_GetStatus(GPTR This, GPTR pdwStatus)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (!pdwStatus) return DSERR_INVALIDPARAM;
    uint32_t st = (b->playing ? DSBSTATUS_PLAYING : 0) | (b->looping ? DSBSTATUS_LOOPING : 0);
    put32(pdwStatus, st);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_Initialize(GPTR This, GPTR pDirectSound, GPTR pcDSBufferDesc)
{
    (void)pDirectSound;
    (void)pcDSBufferDesc;
    if (!buf_of(This)) return DSERR_UNSUPPORTED;
    return DSERR_ALREADYINITIALIZED;              /* buffers come from CreateSoundBuffer, already initialized */
}

HRESULT WINAPI IDirectSoundBuffer_Lock(GPTR This, DWORD dwOffset, DWORD dwBytes, GPTR ppvAudioPtr1,
                                       GPTR pdwAudioBytes1, GPTR ppvAudioPtr2, GPTR pdwAudioBytes2,
                                       DWORD dwFlags)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    put32(ppvAudioPtr1, 0);
    put32(pdwAudioBytes1, 0);
    put32(ppvAudioPtr2, 0);
    put32(pdwAudioBytes2, 0);
    uint32_t off = dwOffset, n = dwBytes;
    if (dwFlags & DSBLOCK_FROMWRITECURSOR) off = write_cursor(b);
    if (dwFlags & DSBLOCK_ENTIREBUFFER) {
        off = 0;
        n = b->bytes;
    }
    if (dsound_trace())
        VT3_INFO("DirectSound: Lock 0x%08x offset %u bytes %u flags 0x%x (play %u) t=%llu ms thr %llu", (unsigned)This, (unsigned)off,
                 (unsigned)n, (unsigned)dwFlags, (unsigned)play_cursor(b),
                 (unsigned long long)(now_ns() / 1000000u % 100000u), (unsigned long long)((uintptr_t)pthread_self() % 1000u));
    if (!b->bytes || off >= b->bytes || n > b->bytes) return DSERR_INVALIDPARAM;
    uint32_t n1 = n, n2 = 0;
    if (off + n > b->bytes) {                     /* the region wraps past the end of the buffer */
        n1 = b->bytes - off;
        n2 = n - n1;
    }
    put32(ppvAudioPtr1, b->storage + off);
    put32(pdwAudioBytes1, n1);
    if (n2) {
        put32(ppvAudioPtr2, b->storage);
        put32(pdwAudioBytes2, n2);
    }
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_Play(GPTR This, DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    (void)dwReserved1;
    (void)dwPriority;
    if (dsound_trace())
        VT3_INFO("DirectSound: Play 0x%08x (%u bytes, flags 0x%x, pos %u)", (unsigned)This, (unsigned)b->bytes,
                 (unsigned)dwFlags, (unsigned)b->pos);
    pthread_mutex_lock(&g_mu);
    b->looping = (dwFlags & DSBPLAY_LOOPING) != 0;
    if (!b->playing) {
        b->playing = 1;
        b->play_base = b->pos;
        b->play_ns = now_ns();
        active_add_locked(b);
    }
    pthread_mutex_unlock(&g_mu);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_SetCurrentPosition(GPTR This, DWORD dwNewPosition)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (dwNewPosition >= b->bytes) return DSERR_INVALIDPARAM;
    if (dsound_trace())
        VT3_INFO("DirectSound: SetCurrentPosition 0x%08x to %u (playing %d)", (unsigned)This, (unsigned)dwNewPosition,
                 b->playing);
    b->pos = dwNewPosition;
    if (b->playing) {
        b->play_base = dwNewPosition;
        b->play_ns = now_ns();
    }
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_SetFormat(GPTR This, GPTR pcfxFormat)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    const uint8_t *f = pcfxFormat ? d3d9_gmem(pcfxFormat, WFX_BYTES) : NULL;
    if (!f) return DSERR_INVALIDPARAM;
    memcpy(b->fmt, f, WFX_BYTES);
    b->freq = rd32(f, 4);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_SetVolume(GPTR This, INT lVolume)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (lVolume > DSBVOLUME_MAX || lVolume < DSBVOLUME_MIN) return DSERR_INVALIDPARAM;
    b->volume = lVolume;
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_SetPan(GPTR This, INT lPan)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (lPan > DSBPAN_RIGHT || lPan < DSBPAN_LEFT) return DSERR_INVALIDPARAM;
    b->pan = lPan;
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_SetFrequency(GPTR This, DWORD dwFrequency)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (dwFrequency == DSBFREQUENCY_ORIGINAL) dwFrequency = rd32(b->fmt, 4);
    if (dwFrequency < DSBFREQUENCY_MIN || dwFrequency > DSBFREQUENCY_MAX) return DSERR_INVALIDPARAM;
    b->freq = dwFrequency;
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_Stop(GPTR This)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (dsound_trace())
        VT3_INFO("DirectSound: Stop 0x%08x (playing %d)", (unsigned)This, b->playing);
    pthread_mutex_lock(&g_mu);
    if (b->playing) {
        b->pos = play_cursor(b);
        b->playing = 0;
        b->looping = 0;                  /* the looping status belongs to the playback that Stop ends */
        active_remove_locked(b);
    }
    pthread_mutex_unlock(&g_mu);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_Unlock(GPTR This, GPTR pvAudioPtr1, DWORD dwAudioBytes1, GPTR pvAudioPtr2,
                                         DWORD dwAudioBytes2)
{
    (void)pvAudioPtr1;
    (void)dwAudioBytes1;
    (void)pvAudioPtr2;
    (void)dwAudioBytes2;
    return buf_of(This) ? DS_OK : DSERR_UNSUPPORTED;
}

HRESULT WINAPI IDirectSoundBuffer_Restore(GPTR This)
{
    return buf_of(This) ? DS_OK : DSERR_UNSUPPORTED;
}
