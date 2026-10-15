/*
 * android_main: the NativeActivity entry point of the Android build. It attaches the glue to the
 * platform layer and runs the translated game on this thread. The game's data and image come from
 * VT3_ANDROID_ROOT (device storage, pushed with adb), so nothing is bundled in the APK.
 */
#include "vt3/vt3_log.h"
#include "vt3/vt3_platform.h"
#include "vt3rt.h"

#include <android_native_app_glue.h>
#include <stdlib.h>

/* Defined in src/platform/platform_android.c */
void vt3_platform_android_attach(struct android_app *app);

#define VT3_ANDROID_ROOT "/sdcard/Android/data/net.vt3port.game/files"

void android_main(struct android_app *app)
{
    vt3_platform_android_attach(app);
    /* Game data: <root>/data2 is the extracted English archive set; <root>/image.bin is the image. */
    setenv("VT3_GAME_ROOT", VT3_ANDROID_ROOT "/data2", 1);
    setenv("VT3_SHADER_PACK", VT3_ANDROID_ROOT "/shaders.pack", 1);
    setenv("VT3_TRACE_FPS", "1", 0);   /* frame rate once a second in logcat, while the Android port is young */
    VT3_INFO("android: running the game from %s", VT3_ANDROID_ROOT);
    int rc = vt3_run(&vt3_mod, VT3_ANDROID_ROOT "/image.bin");
    VT3_INFO("android: game returned %d", rc);
}
