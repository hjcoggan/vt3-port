/*
 * vt3rt: the runtime's test harness.
 *
 * Opens a window, clears to a colour, draws one quad, plays a 1-second
 * 440 Hz tone, and exits when the window closes, Escape is pressed, or
 * --frames N is reached. --selftest runs the checks in tests/selftest.c.
 */
#include "selftest.h"
#include "vt3/vt3_audio.h"
#include "vt3/vt3_guest.h"
#include "vt3/vt3_input.h"
#include "vt3/vt3_log.h"
#include "vt3/vt3_platform.h"
#include "vt3/vt3_renderer.h"
#include "vt3shim/shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TONE_HZ            440.0
#define TONE_SECONDS       1
#define AUDIO_RATE         48000u
#define AUDIO_CHANNELS     2
#define AUDIO_BUFFER_BYTES (AUDIO_RATE / 2 * AUDIO_CHANNELS * sizeof(int16_t))  /* 0.5 s */

static void usage(const char *argv0)
{
    printf("usage: %s [options]\n"
           "       %s --selftest [ADX_OR_XPAC_PATH]\n"
           "\n"
           "  --window WxH       window size in points (default 640x480)\n"
           "  --logical WxH      game resolution (default 640x480)\n"
           "  --aspect W:H       display aspect (default 4:3)\n"
           "  --integer          integer scaling where it fits\n"
           "  --frames N         exit after N frames (default: until quit)\n"
           "  --no-audio         do not open the audio device\n"
           "  --no-vsync         disable vsync\n"
           "  --game-root PATH   directory for game data (Win32 paths map onto it)\n"
           "  --selftest [PATH]  run the self-test; PATH is an .adx or .xpac for the ADX check\n",
           argv0, argv0);
}

static int parse_pair(const char *s, char sep, int *a, int *b)
{
    char *end = NULL;
    long x = strtol(s, &end, 10);
    if (end == s || *end != sep) return -1;
    const char *rest = end + 1;
    long y = strtol(rest, &end, 10);
    if (end == rest || *end != '\0' || x <= 0 || y <= 0) return -1;
    *a = (int)x;
    *b = (int)y;
    return 0;
}

static void draw_test_quad(void)
{
    /* Logical coordinates (640x480). XYZRHW, D3DCOLOR is 0xAARRGGBB. */
    const vt3_tlvertex q[6] = {
        { 160, 120, 0, 1, 0xFFFF0000u, 0, 0 },   /* top-left: red */
        { 480, 120, 0, 1, 0xFF00FF00u, 1, 0 },   /* top-right: green */
        { 480, 360, 0, 1, 0xFF0000FFu, 1, 1 },   /* bottom-right: blue */
        { 160, 120, 0, 1, 0xFFFF0000u, 0, 0 },
        { 480, 360, 0, 1, 0xFF0000FFu, 1, 1 },
        { 160, 360, 0, 1, 0xFFFFFFFFu, 0, 1 },   /* bottom-left: white */
    };
    vt3_gfx_draw(VT3_PRIM_TRIANGLELIST, q, 6, 0);
}

int main(int argc, char **argv)
{
    vt3_config cfg;
    vt3_config_defaults(&cfg);

    int run_selftest = 0;
    const char *selftest_path = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int ok = 1;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(a, "--selftest") == 0) {
            run_selftest = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') selftest_path = argv[++i];
        } else if (strcmp(a, "--window") == 0 && i + 1 < argc) {
            ok = parse_pair(argv[++i], 'x', &cfg.window_w, &cfg.window_h) == 0;
        } else if (strcmp(a, "--logical") == 0 && i + 1 < argc) {
            ok = parse_pair(argv[++i], 'x', &cfg.logical_w, &cfg.logical_h) == 0;
        } else if (strcmp(a, "--aspect") == 0 && i + 1 < argc) {
            ok = parse_pair(argv[++i], ':', &cfg.aspect_w, &cfg.aspect_h) == 0;
        } else if (strcmp(a, "--integer") == 0) {
            cfg.integer_scale = 1;
        } else if (strcmp(a, "--frames") == 0 && i + 1 < argc) {
            cfg.frames = atoi(argv[++i]);
        } else if (strcmp(a, "--no-audio") == 0) {
            cfg.audio = 0;
        } else if (strcmp(a, "--no-vsync") == 0) {
            cfg.vsync = 0;
        } else if (strcmp(a, "--game-root") == 0 && i + 1 < argc) {
            cfg.game_root = argv[++i];
        } else {
            ok = 0;
        }
        if (!ok) {
            fprintf(stderr, "bad argument: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (run_selftest) return vt3_selftest_run(selftest_path);

    if (vt3_guest_init(VT3_GUEST_DEFAULT_BYTES) != 0) return 1;
    if (vt3shim_init() != 0) return 1;
    vt3shim_set_game_root(cfg.game_root);

    if (vt3_platform_init(&cfg) != 0) {
        vt3shim_shutdown();
        vt3_guest_shutdown();
        return 1;
    }
    vt3_input_init();

    vt3_gfx_config gc = {
        .logical_w = cfg.logical_w,
        .logical_h = cfg.logical_h,
        .aspect_w = cfg.aspect_w,
        .aspect_h = cfg.aspect_h,
        .integer_scale = cfg.integer_scale,
    };
    if (vt3_gfx_init(&vt3_gfx_gl33, &gc) != 0) {
        vt3_platform_shutdown();
        vt3shim_shutdown();
        vt3_guest_shutdown();
        return 1;
    }

    vt3_audio *audio = NULL;
    if (cfg.audio) {
        vt3_audio_format fmt = { AUDIO_RATE, AUDIO_CHANNELS };
        if (vt3_audio_open(&audio, fmt, AUDIO_BUFFER_BYTES) == 0) {
            vt3_audio_set_playing(audio, 1);
        } else {
            VT3_WARN("continuing without audio");
            audio = NULL;
        }
    }

    double phase = 0.0;
    size_t tone_frames_left = (size_t)AUDIO_RATE * TONE_SECONDS;
    int frame = 0;
    while (vt3_platform_pump_events()) {
        int dw = 0, dh = 0;
        vt3_platform_drawable_size(&dw, &dh);

        vt3_gfx_begin_frame();
        vt3_gfx_clear(0xFF1B3A4Bu);   /* the colour the runtime clears to */
        draw_test_quad();
        vt3_gfx_present(dw, dh);
        vt3_platform_swap();

        if (audio && tone_frames_left) {
            size_t n = vt3_audio_fill_sine(audio, TONE_HZ, &phase, tone_frames_left, 6000);
            tone_frames_left -= n;
        }

        frame++;
        if (cfg.frames && frame >= cfg.frames) break;
    }

    VT3_INFO("exiting after %d frames; audio underruns %u", frame,
             audio ? vt3_audio_underruns(audio) : 0u);

    vt3_audio_close(audio);
    vt3_gfx_shutdown();
    vt3_platform_shutdown();
    vt3shim_shutdown();
    vt3_guest_shutdown();
    return 0;
}
