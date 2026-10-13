/* Host entry for a translated module. Usage: prog [path/to/image.bin] */
#include "vt3rt.h"

int main(int argc, char **argv)
{
    const char *img = argc > 1 ? argv[1] : vt3_mod.image_file;
    return vt3_run(&vt3_mod, img);
}
