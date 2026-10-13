/*
 * CRI ADX (ADPCM) decoder, public interface.
 *
 * Written from the public format description (header layout and the
 * predictor described on the multimedia.cx and Wikipedia pages). No
 * third-party code. See runtime/README.md for the verification method.
 *
 * Supported: encoding type 3 (standard ADX), 18-byte blocks, 4-bit
 * samples, 1 to VT3_ADX_MAX_CHANNELS channels, 16-bit output.
 * Not yet supported: encoding type 4 (exponential scale) and type 2
 * (fixed coefficients). These return VT3_ADX_ERR_UNSUPPORTED.
 */
#ifndef VT3_ADX_H
#define VT3_ADX_H

#include <stddef.h>
#include <stdint.h>

#define VT3_ADX_BLOCK_BYTES        18  /* per channel: 2-byte scale + 16 bytes of nibbles */
#define VT3_ADX_SAMPLES_PER_BLOCK  32
#define VT3_ADX_MAX_CHANNELS       8
#define VT3_ADX_MIN_HEADER_BYTES   20

enum {
    VT3_ADX_OK                 = 0,
    VT3_ADX_END_OF_STREAM      = 1,   /* returned by decode_frame for the end marker */
    VT3_ADX_ERR_ARG            = -1,
    VT3_ADX_ERR_SHORT          = -2,  /* buffer too small for header or data */
    VT3_ADX_ERR_MAGIC          = -3,  /* first two bytes are not 0x80 0x00 */
    VT3_ADX_ERR_UNSUPPORTED    = -4,  /* encoding type, block size, or bits not handled */
    VT3_ADX_ERR_CHANNELS       = -5
};

typedef struct vt3_adx_header {
    int      encoding;       /* byte 0x04, 3 = standard ADX */
    int      block_size;     /* byte 0x05, 18 */
    int      bits;           /* byte 0x06, 4 */
    int      channels;       /* byte 0x07 */
    uint32_t sample_rate;    /* bytes 0x08..0x0B, big-endian */
    uint32_t total_samples;  /* bytes 0x0C..0x0F, samples per channel */
    int      highpass_hz;    /* bytes 0x10..0x11 */
    int      version;        /* byte 0x12 */
    size_t   data_offset;    /* copyright offset (bytes 0x02..0x03) + 4 */
} vt3_adx_header;

typedef struct vt3_adx_decoder {
    vt3_adx_header hdr;
    int32_t  coef[2];                        /* predictor coefficients, fixed point */
    int32_t  s1[VT3_ADX_MAX_CHANNELS];       /* history: previous sample per channel */
    int32_t  s2[VT3_ADX_MAX_CHANNELS];       /* history: sample before that */
} vt3_adx_decoder;

/* Parse the 20+ byte header. Does not require the data to be present. */
int vt3_adx_parse_header(const uint8_t *buf, size_t len, vt3_adx_header *out);

/* Predictor coefficients from highpass cutoff and sample rate. */
void vt3_adx_compute_coefs(int highpass_hz, uint32_t sample_rate, int32_t out[2]);

/* Duration in seconds from the header (total_samples / sample_rate). */
double vt3_adx_duration_seconds(const vt3_adx_header *h);

/* Initialise a streaming decoder from the full stream header. */
int vt3_adx_decoder_init(vt3_adx_decoder *d, const uint8_t *buf, size_t len);

/*
 * Decode one frame: one 18-byte block per channel, in channel order.
 * `frame` must hold channels * 18 bytes. Writes 32 * channels interleaved
 * samples to `out`. Returns VT3_ADX_OK, VT3_ADX_END_OF_STREAM, or an error.
 */
int vt3_adx_decode_frame(vt3_adx_decoder *d, const uint8_t *frame, int16_t *out);

/*
 * Decode a whole in-memory stream. Writes interleaved 16-bit samples to
 * `out`, at most `max_samples` per channel (so out needs room for
 * max_samples * channels values). Stops at total_samples or the end marker.
 * Returns the number of samples per channel written, or a negative error.
 */
int64_t vt3_adx_decode_all(const uint8_t *buf, size_t len, int16_t *out,
                           size_t max_samples, vt3_adx_header *hdr_out);

#endif /* VT3_ADX_H */
