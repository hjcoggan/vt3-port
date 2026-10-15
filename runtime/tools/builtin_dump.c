/* Host tool: writes the built-in Vulkan GLSL into a directory for the shader pack. Usage: vt3builtin DIR */
#include "vt3/vt3_builtin.h"

#include <stdio.h>

static int put(const char *dir, const char *name, const char *text)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    fputs(text, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: vt3builtin DIR\n");
        return 2;
    }
    int rc = 0;
    rc |= put(argv[1], "ff_fs.glsl", vt3_builtin_ff_fs_vk);
    rc |= put(argv[1], "tl_vs.glsl", vt3_builtin_tl_vs_vk);
    rc |= put(argv[1], "tl_fs.glsl", vt3_builtin_tl_fs_vk);
    return rc;
}
