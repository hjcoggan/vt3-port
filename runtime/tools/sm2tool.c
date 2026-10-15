/*
 * Host tool for the shader pack: translates one SM2 shader binary to GLSL 450 for SPIR-V.
 * Usage: vt3sm2 IN.bin OUT.glsl. The binary is the token stream (little-endian u32, version
 * token first). tools/formats/shaders/gen_spirv_pack.py runs it over every game shader.
 */
#include "vt3/vt3_sm2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: vt3sm2 IN.bin OUT.glsl\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "vt3sm2: cannot open %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 8 || (n & 3)) {
        fprintf(stderr, "vt3sm2: %s is not a token stream\n", argv[1]);
        fclose(f);
        return 1;
    }
    /* The file is little-endian and so is this host, so the words are read as stored. */
    uint32_t *tok = malloc((size_t)n);
    if (!tok || fread(tok, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return 1;
    }
    fclose(f);
    vt3_sm2_shader sh;
    if (vt3_sm2_translate_ex(tok, (size_t)n / 4, &sh, VT3_SM2_FLAG_VULKAN) != 0) {
        fprintf(stderr, "vt3sm2: %s: %s\n", argv[1], sh.error);
        free(tok);
        return 1;
    }
    FILE *o = fopen(argv[2], "wb");
    if (!o || fwrite(sh.glsl, 1, strlen(sh.glsl), o) != strlen(sh.glsl)) {
        fprintf(stderr, "vt3sm2: cannot write %s\n", argv[2]);
        return 1;
    }
    fclose(o);
    vt3_sm2_free(&sh);
    free(tok);
    return 0;
}
