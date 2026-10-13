/*
 * Audio output on SDL3. SDL calls our callback on its audio thread when the
 * device needs data. The callback drains the ring that the game-side writer
 * fills. Underruns come out as silence and are counted.
 */
#include "vt3/vt3_audio.h"
#include "vt3/vt3_log.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct vt3_audio {
    SDL_AudioStream *stream;
    vt3_ringbuf *rb;
    vt3_audio_format fmt;
    size_t frame_bytes;
    _Atomic uint64_t played;
    _Atomic uint32_t underruns;
    _Atomic int playing;
};

static void SDLCALL pull_cb(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
    (void)total;
    vt3_audio *a = (vt3_audio *)userdata;
    uint8_t chunk[4096];
    while (additional > 0) {
        size_t want = (size_t)additional < sizeof chunk ? (size_t)additional : sizeof chunk;
        size_t got = 0;
        if (atomic_load(&a->playing)) {
            got = vt3_rb_read(a->rb, chunk, want);
            /* Only a partial read counts: the writer fell behind mid-stream.
             * An empty ring (startup, or after the tone ends) is silence. */
            if (got > 0 && got < want) {
                atomic_fetch_add(&a->underruns, 1);
            }
            atomic_fetch_add(&a->played, got);
        }
        if (got < want) memset(chunk + got, 0, want - got);
        SDL_PutAudioStreamData(stream, chunk, (int)want);
        additional -= (int)want;
    }
}

int vt3_audio_open(vt3_audio **out, vt3_audio_format fmt, size_t buffer_bytes)
{
    *out = NULL;
    if (fmt.channels < 1 || fmt.channels > 2 || fmt.sample_rate == 0) return -1;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        VT3_WARN("audio: SDL_InitSubSystem failed: %s", SDL_GetError());
        return -1;
    }

    vt3_audio *a = calloc(1, sizeof *a);
    if (!a) return -1;
    a->fmt = fmt;
    a->frame_bytes = (size_t)fmt.channels * sizeof(int16_t);
    a->rb = vt3_rb_create(buffer_bytes);
    if (!a->rb) {
        free(a);
        return -1;
    }

    SDL_AudioSpec spec;
    memset(&spec, 0, sizeof spec);
    spec.format = SDL_AUDIO_S16;
    spec.channels = fmt.channels;
    spec.freq = (int)fmt.sample_rate;

    a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, pull_cb, a);
    if (!a->stream) {
        VT3_WARN("audio: no playback device (%s)", SDL_GetError());
        vt3_rb_destroy(a->rb);
        free(a);
        return -1;
    }
    SDL_ResumeAudioStreamDevice(a->stream);
    *out = a;
    VT3_INFO("audio: %u Hz, %d ch, S16, ring %zu bytes", fmt.sample_rate, fmt.channels,
             vt3_rb_capacity(a->rb));
    return 0;
}

void vt3_audio_close(vt3_audio *a)
{
    if (!a) return;
    SDL_DestroyAudioStream(a->stream);   /* stops the callback before we free the ring */
    vt3_rb_destroy(a->rb);
    free(a);
}

size_t vt3_audio_write(vt3_audio *a, const void *pcm, size_t bytes)
{
    size_t whole = bytes - (bytes % a->frame_bytes);
    return vt3_rb_write(a->rb, pcm, whole);
}

size_t vt3_audio_space(const vt3_audio *a)
{
    return vt3_rb_space(a->rb);
}

uint64_t vt3_audio_played_bytes(const vt3_audio *a)
{
    return atomic_load(&a->played);
}

void vt3_audio_set_playing(vt3_audio *a, int playing)
{
    atomic_store(&a->playing, playing ? 1 : 0);
}

uint32_t vt3_audio_underruns(const vt3_audio *a)
{
    return atomic_load(&a->underruns);
}

size_t vt3_audio_fill_sine(vt3_audio *a, double freq_hz, double *phase,
                           size_t max_frames, int16_t amplitude)
{
    const double two_pi = 6.283185307179586;
    const double step = two_pi * freq_hz / (double)a->fmt.sample_rate;
    size_t space_frames = vt3_audio_space(a) / a->frame_bytes;
    size_t frames = max_frames < space_frames ? max_frames : space_frames;
    if (frames == 0) return 0;

    int16_t buf[4096 * 2];
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done;
        if (n > 4096) n = 4096;
        for (size_t i = 0; i < n; i++) {
            int16_t s = (int16_t)(sin(*phase) * amplitude);
            for (int c = 0; c < a->fmt.channels; c++) buf[i * a->fmt.channels + c] = s;
            *phase += step;
            if (*phase > two_pi) *phase -= two_pi;
        }
        vt3_audio_write(a, buf, n * a->frame_bytes);
        done += n;
    }
    return done;
}
