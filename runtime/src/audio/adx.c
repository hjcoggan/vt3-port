/*
 * CRI ADX decoder (encoding type 3). See include/vt3/vt3_adx.h.
 *
 * Stream layout (all header integers big-endian):
 *   0x00  0x80 0x00           signature
 *   0x02  u16                 copyright offset; audio starts at this + 4
 *   0x04  u8                  encoding type (3 = standard ADX)
 *   0x05  u8                  block size (18)
 *   0x06  u8                  bits per sample (4)
 *   0x07  u8                  channels
 *   0x08  u32                 sample rate
 *   0x0C  u32                 total samples per channel
 *   0x10  u16                 highpass cutoff (Hz), used for predictor coefficients
 *   0x12  u8                  version
 *
 * Data: frames of one 18-byte block per channel, channel order. A block is
 * a u16 scale followed by 32 signed 4-bit samples (high nibble first).
 * A frame whose scale has the top bit set marks end of stream.
 *
 * Prediction (per channel, state carried across blocks):
 *   pred = (c0 * s1 + c1 * s2) >> SHIFT
 *   s0   = nibble * scale + pred, clamped to int16
 * with c0 = 2c and c1 = -c*c, where
 *   a = sqrt(2) - cos(2*pi*hpf/sr),  b = sqrt(2) - 1,
 *   c = (a - sqrt((a+b)*(a-b))) / b.
 */
#include "vt3/vt3_adx.h"

#include <math.h>
#include <string.h>

/* Fixed-point precision of the predictor coefficients. The public material
 * disagrees (12 vs 14 bits). 12 bits is the value that matches ffmpeg
 * bit-for-bit on 321 streams (see runtime/README.md, "ADX verification"). */
#define VT3_ADX_COEF_SHIFT 12
/* Round coefficients to nearest. Floor (as one public description says)
 * differs from ffmpeg on the 44.1 kHz streams. */
#define VT3_ADX_COEF_ROUND 1
/* The 15-bit scale is used as-is. Adding 1 to it differs from ffmpeg. */
#define VT3_ADX_SCALE_BIAS 0

static uint16_t rd_be16(const uint8_t *p) { return (uint16_t)(((unsigned)p[0] << 8) | p[1]); }

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int32_t clamp16(int64_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int32_t)v;
}

void vt3_adx_compute_coefs(int highpass_hz, uint32_t sample_rate, int32_t out[2])
{
    if (sample_rate == 0) {
        out[0] = 0;
        out[1] = 0;
        return;
    }
    const double pi = 3.14159265358979323846;
    const double root2 = sqrt(2.0);
    double a = root2 - cos(2.0 * pi * (double)highpass_hz / (double)sample_rate);
    double b = root2 - 1.0;
    double c = (a - sqrt((a + b) * (a - b))) / b;
    double c0 = 2.0 * c;
    double c1 = -c * c;
    double scale = (double)(1 << VT3_ADX_COEF_SHIFT);
#if VT3_ADX_COEF_ROUND
    out[0] = (int32_t)floor(c0 * scale + 0.5);
    out[1] = (int32_t)floor(c1 * scale + 0.5);
#else
    out[0] = (int32_t)floor(c0 * scale);
    out[1] = (int32_t)floor(c1 * scale);
#endif
}

double vt3_adx_duration_seconds(const vt3_adx_header *h)
{
    if (!h || h->sample_rate == 0) return 0.0;
    return (double)h->total_samples / (double)h->sample_rate;
}

int vt3_adx_parse_header(const uint8_t *buf, size_t len, vt3_adx_header *h)
{
    if (!buf || !h) return VT3_ADX_ERR_ARG;
    if (len < VT3_ADX_MIN_HEADER_BYTES) return VT3_ADX_ERR_SHORT;
    if (buf[0] != 0x80 || buf[1] != 0x00) return VT3_ADX_ERR_MAGIC;

    vt3_adx_header t;
    memset(&t, 0, sizeof t);
    t.data_offset = (size_t)rd_be16(buf + 0x02) + 4;
    t.encoding = buf[0x04];
    t.block_size = buf[0x05];
    t.bits = buf[0x06];
    t.channels = buf[0x07];
    t.sample_rate = rd_be32(buf + 0x08);
    t.total_samples = rd_be32(buf + 0x0C);
    t.highpass_hz = rd_be16(buf + 0x10);
    t.version = buf[0x12];

    if (t.encoding != 3) return VT3_ADX_ERR_UNSUPPORTED;
    if (t.block_size != VT3_ADX_BLOCK_BYTES || t.bits != 4) return VT3_ADX_ERR_UNSUPPORTED;
    if (t.channels < 1 || t.channels > VT3_ADX_MAX_CHANNELS) return VT3_ADX_ERR_CHANNELS;
    if (t.sample_rate == 0) return VT3_ADX_ERR_UNSUPPORTED;
    if (t.data_offset > len) return VT3_ADX_ERR_SHORT;

    *h = t;
    return VT3_ADX_OK;
}

int vt3_adx_decoder_init(vt3_adx_decoder *d, const uint8_t *buf, size_t len)
{
    if (!d) return VT3_ADX_ERR_ARG;
    int rc = vt3_adx_parse_header(buf, len, &d->hdr);
    if (rc != VT3_ADX_OK) return rc;
    vt3_adx_compute_coefs(d->hdr.highpass_hz, d->hdr.sample_rate, d->coef);
    memset(d->s1, 0, sizeof d->s1);
    memset(d->s2, 0, sizeof d->s2);
    return VT3_ADX_OK;
}

int vt3_adx_decode_frame(vt3_adx_decoder *d, const uint8_t *frame, int16_t *out)
{
    if (!d || !frame || !out) return VT3_ADX_ERR_ARG;
    const int ch_count = d->hdr.channels;

    /* End marker: top bit of any channel's scale. */
    for (int c = 0; c < ch_count; c++) {
        if (rd_be16(frame + c * VT3_ADX_BLOCK_BYTES) & 0x8000u) return VT3_ADX_END_OF_STREAM;
    }

    for (int c = 0; c < ch_count; c++) {
        const uint8_t *blk = frame + c * VT3_ADX_BLOCK_BYTES;
        const int32_t scale = (int32_t)(rd_be16(blk) & 0x7FFFu) + VT3_ADX_SCALE_BIAS;
        int32_t s1 = d->s1[c];
        int32_t s2 = d->s2[c];

        for (int i = 0; i < VT3_ADX_SAMPLES_PER_BLOCK; i++) {
            uint8_t byte = blk[2 + i / 2];
            int nib = (i & 1) ? (byte & 0x0F) : (byte >> 4);
            int32_t dn = (nib ^ 8) - 8; /* sign-extend 4 bits */
            int64_t pred = ((int64_t)d->coef[0] * s1 + (int64_t)d->coef[1] * s2) >> VT3_ADX_COEF_SHIFT;
            int32_t s0 = clamp16((int64_t)dn * scale + pred);
            out[i * ch_count + c] = (int16_t)s0;
            s2 = s1;
            s1 = s0;
        }
        d->s1[c] = s1;
        d->s2[c] = s2;
    }
    return VT3_ADX_OK;
}

int64_t vt3_adx_decode_all(const uint8_t *buf, size_t len, int16_t *out,
                           size_t max_samples, vt3_adx_header *hdr_out)
{
    if (!buf || !out) return VT3_ADX_ERR_ARG;
    vt3_adx_decoder d;
    int rc = vt3_adx_decoder_init(&d, buf, len);
    if (rc != VT3_ADX_OK) return rc;
    if (hdr_out) *hdr_out = d.hdr;

    const int ch_count = d.hdr.channels;
    const size_t frame_bytes = (size_t)VT3_ADX_BLOCK_BYTES * ch_count;
    const size_t want = d.hdr.total_samples < max_samples ? d.hdr.total_samples : max_samples;

    int16_t tmp[VT3_ADX_SAMPLES_PER_BLOCK * VT3_ADX_MAX_CHANNELS];
    size_t done = 0;
    size_t pos = d.hdr.data_offset;

    while (done < want) {
        if (pos + frame_bytes > len) break;   /* truncated stream */
        rc = vt3_adx_decode_frame(&d, buf + pos, tmp);
        if (rc == VT3_ADX_END_OF_STREAM) break;
        if (rc != VT3_ADX_OK) return rc;
        size_t take = want - done;
        if (take > VT3_ADX_SAMPLES_PER_BLOCK) take = VT3_ADX_SAMPLES_PER_BLOCK;
        memcpy(out + done * ch_count, tmp, take * ch_count * sizeof(int16_t));
        done += take;
        pos += frame_bytes;
    }
    return (int64_t)done;
}
