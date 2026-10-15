#include "vt3/vt3_log.h"

#include <stdarg.h>
#include <stdio.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

void vt3_log(vt3_log_level level, const char *fmt, ...)
{
    static const char *const tag[] = { "debug", "info", "warn", "error" };
#ifdef __ANDROID__
    /* Android drops a native app's stderr, so the log goes to logcat under the vt3 tag. */
    static const android_LogPriority prio[] = { ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN,
                                                ANDROID_LOG_ERROR };
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(prio[level], "vt3", fmt, ap);
    va_end(ap);
    (void)tag;
#else
    fprintf(stderr, "[vt3:%s] ", tag[level]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
#endif
}
