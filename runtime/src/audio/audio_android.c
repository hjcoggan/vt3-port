/*
 * Audio output on Android through AAudio. A data callback on AAudio's thread drains the ring
 * that the game-side writer fills; an empty or short ring is silence. Same contract as
 * audio_sdl.c (vt3_audio.h).
 */
#include "vt3/vt3_audio.h"
#include "vt3/vt3_log.h"

#include <aaudio/AAudio.h>

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct vt3_audio {
    AAudioStream *stream;
    vt3_ringbuf *rb;
    vt3_audio_format fmt;
    size_t frame_bytes;
    _Atomic int playing;
    _Atomic uint32_t underruns;
    _Atomic uint64_t played;
};

static aaudio_data_callback_result_t data_callback(AAudioStream *stream, void *user, void *audio,
                                                   int32_t num_frames)
{
    (void)stream;
    vt3_audio *a = user;
    size_t want = (size_t)num_frames * a->frame_bytes;
    size_t got = 0;
    if (atomic_load(&a->playing)) {
        got = vt3_rb_read(a->rb, audio, want);
        /* A short read mid-stream means the writer fell behind; the rest of the buffer is silence. */
        if (got < want) atomic_fetch_add(&a->underruns, 1);
        atomic_fetch_add(&a->played, got);
    }
    if (got < want) memset((uint8_t *)audio + got, 0, want - got);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

int vt3_audio_open(vt3_audio **out, vt3_audio_format fmt, size_t buffer_bytes)
{
    *out = NULL;
    vt3_audio *a = calloc(1, sizeof *a);
    if (!a) return -1;
    a->fmt = fmt;
    a->frame_bytes = (size_t)fmt.channels * sizeof(int16_t);
    a->rb = vt3_rb_create(buffer_bytes);
    if (!a->rb) { free(a); return -1; }

    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) {
        VT3_ERROR("audio: AAudio_createStreamBuilder failed");
        vt3_rb_destroy(a->rb);
        free(a);
        return -1;
    }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)fmt.sample_rate);
    AAudioStreamBuilder_setChannelCount(b, fmt.channels);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(b, data_callback, a);
    aaudio_result_t rc = AAudioStreamBuilder_openStream(b, &a->stream);
    AAudioStreamBuilder_delete(b);
    if (rc != AAUDIO_OK || !a->stream) {
        VT3_ERROR("audio: AAudio open failed: %s", AAudio_convertResultToText(rc));
        vt3_rb_destroy(a->rb);
        free(a);
        return -1;
    }
    rc = AAudioStream_requestStart(a->stream);
    if (rc != AAUDIO_OK) {
        VT3_ERROR("audio: AAudio start failed: %s", AAudio_convertResultToText(rc));
        AAudioStream_close(a->stream);
        vt3_rb_destroy(a->rb);
        free(a);
        return -1;
    }
    VT3_INFO("audio: %u Hz, %d ch, S16 (AAudio), ring %zu bytes, device burst %d frames",
             fmt.sample_rate, fmt.channels, buffer_bytes, AAudioStream_getFramesPerBurst(a->stream));
    *out = a;
    return 0;
}

void vt3_audio_close(vt3_audio *a)
{
    if (!a) return;
    if (a->stream) {
        AAudioStream_requestStop(a->stream);
        AAudioStream_close(a->stream);
    }
    vt3_rb_destroy(a->rb);
    free(a);
}

size_t vt3_audio_write(vt3_audio *a, const void *pcm, size_t bytes)
{
    return vt3_rb_write(a->rb, pcm, bytes);
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

size_t vt3_audio_fill_sine(vt3_audio *a, double freq_hz, double *phase, size_t max_frames, int16_t amplitude)
{
    size_t space_frames = vt3_rb_space(a->rb) / a->frame_bytes;
    size_t frames = max_frames < space_frames ? max_frames : space_frames;
    int16_t buf[2 * 4096];
    if (frames > 4096) frames = 4096;
    int ch = a->fmt.channels;
    double step = 2.0 * M_PI * freq_hz / (double)a->fmt.sample_rate;
    for (size_t i = 0; i < frames; i++) {
        int16_t v = (int16_t)(sin(*phase) * amplitude);
        for (int c = 0; c < ch; c++) buf[i * ch + c] = v;
        *phase += step;
        if (*phase > 2.0 * M_PI) *phase -= 2.0 * M_PI;
    }
    return vt3_rb_write(a->rb, buf, frames * a->frame_bytes) / a->frame_bytes;
}
