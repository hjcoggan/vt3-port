/*
 * android_main: the NativeActivity entry point of the Android build. It attaches the glue to the
 * platform layer and runs the translated game on this thread. The game's data and image come from
 * VT3_ANDROID_ROOT (device storage, pushed with adb), so nothing is bundled in the APK.
 */
#include "vt3/vt3_log.h"
#include "vt3/vt3_platform.h"
#include "vt3rt.h"

#include <android/native_activity.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

/* Defined in src/platform/platform_android.c */
void vt3_platform_android_attach(struct android_app *app);

#define VT3_ANDROID_ROOT "/sdcard/Android/data/net.vt3port.game/files"

/* A device has no shell to set the VT3_* environment in, so read them from a small KEY=VALUE file
 * beside the game data (blank lines and # comments ignored). Every debug switch in the port is an
 * environment variable, and this is how they are reached on the Retroid. Values already in the
 * environment win, so a developer's own setting is never overwritten. */
static void apply_env_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\r' || *s == 0) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *val = eq + 1;
        size_t n = strlen(val);
        while (n && (val[n - 1] == '\n' || val[n - 1] == '\r' || val[n - 1] == ' ' || val[n - 1] == '\t'))
            val[--n] = 0;
        if (getenv(s) == NULL) setenv(s, val, 0);
        VT3_INFO("android: env %s=%s", s, getenv(s));
    }
    fclose(f);
}

#ifdef VT3_PGO_GEN
/* Profile-guided build, collecting: the app is force-stopped rather than exiting, so write the
 * counters every 20 s. */
int __llvm_profile_write_file(void);
void __llvm_profile_set_filename(const char *name);
static void *pgo_dump(void *arg)
{
    (void)arg;
    __llvm_profile_set_filename(VT3_ANDROID_ROOT "/vt3.profraw");
    for (;;) {
        sleep(20);
        __llvm_profile_write_file();
    }
    return NULL;
}
#endif

void android_main(struct android_app *app)
{
#ifdef VT3_PGO_GEN
    pthread_t pgo;
    pthread_create(&pgo, NULL, pgo_dump, NULL);
#endif
    vt3_platform_android_attach(app);
    apply_env_file(VT3_ANDROID_ROOT "/vt3.env");
    /* Full screen: the game draws over the whole panel, with no status bar (the activity has no Java). */
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_FULLSCREEN, 0);
    /* Game data: <root>/data2 is the extracted English archive set; <root>/image.bin is the image. */
    setenv("VT3_GAME_ROOT", VT3_ANDROID_ROOT "/data2", 1);
    setenv("VT3_SHADER_PACK", VT3_ANDROID_ROOT "/shaders.pack", 1);
    VT3_INFO("android: running the game from %s", VT3_ANDROID_ROOT);
    int rc = vt3_run(&vt3_mod, VT3_ANDROID_ROOT "/image.bin");
    VT3_INFO("android: game returned %d", rc);
}
