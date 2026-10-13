#include "vt3/vt3_log.h"

#include <stdarg.h>
#include <stdio.h>

void vt3_log(vt3_log_level level, const char *fmt, ...)
{
    static const char *const tag[] = { "debug", "info", "warn", "error" };
    fprintf(stderr, "[vt3:%s] ", tag[level]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
