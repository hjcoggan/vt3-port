/*
 * Audio. The DirectSound shim feeds PCM through this layer.
 *
 * The ring buffer maps onto a DirectSound secondary buffer: the writer side
 * corresponds to Lock/Unlock, and vt3_audio_played_bytes() to
 * GetCurrentPosition. Output is signed 16-bit interleaved only.
 */
#ifndef VT3_AUDIO_H
#define VT3_AUDIO_H

#include <stddef.h>
#include <stdint.h>

/* Single-producer, single-consumer byte ring. Safe across two threads. */
typedef struct vt3_ringbuf vt3_ringbuf;
vt3_ringbuf *vt3_rb_create(size_t capacity);
void         vt3_rb_destroy(vt3_ringbuf *rb);
size_t       vt3_rb_write(vt3_ringbuf *rb, const void *src, size_t n);  /* bytes accepted */
size_t       vt3_rb_read(vt3_ringbuf *rb, void *dst, size_t n);         /* bytes returned */
size_t       vt3_rb_used(const vt3_ringbuf *rb);
size_t       vt3_rb_space(const vt3_ringbuf *rb);
size_t       vt3_rb_capacity(const vt3_ringbuf *rb);

typedef struct vt3_audio_format {
    uint32_t sample_rate;   /* e.g. 44100 or 48000 */
    int      channels;      /* 1 or 2 */
} vt3_audio_format;

typedef struct vt3_audio vt3_audio;

/* Opens the default playback device through SDL. Returns 0 on success. */
int      vt3_audio_open(vt3_audio **out, vt3_audio_format fmt, size_t buffer_bytes);
void     vt3_audio_close(vt3_audio *a);
size_t   vt3_audio_write(vt3_audio *a, const void *pcm, size_t bytes);
size_t   vt3_audio_space(const vt3_audio *a);
uint64_t vt3_audio_played_bytes(const vt3_audio *a);
void     vt3_audio_set_playing(vt3_audio *a, int playing);
uint32_t vt3_audio_underruns(const vt3_audio *a);

/* Writes up to max_frames frames of a sine wave; *phase carries across calls. */
size_t   vt3_audio_fill_sine(vt3_audio *a, double freq_hz, double *phase,
                             size_t max_frames, int16_t amplitude);

#endif /* VT3_AUDIO_H */
