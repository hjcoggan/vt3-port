/* Entry for the Android self-test executable: run it on the device with adb shell. */
#include "selftest.h"

#include <stddef.h>

int main(int argc, char **argv)
{
    return vt3_selftest_run(argc > 1 ? argv[1] : NULL);
}
