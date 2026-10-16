/* Minimal logging for the runtime and shim. Writes to stderr. */
#ifndef VT3_LOG_H
#define VT3_LOG_H

typedef enum vt3_log_level {
    VT3_LOG_DEBUG = 0,
    VT3_LOG_INFO,
    VT3_LOG_WARN,
    VT3_LOG_ERROR
} vt3_log_level;

void vt3_log(vt3_log_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define VT3_DEBUG(...) vt3_log(VT3_LOG_DEBUG, __VA_ARGS__)
#define VT3_INFO(...)  vt3_log(VT3_LOG_INFO, __VA_ARGS__)
#define VT3_WARN(...)  vt3_log(VT3_LOG_WARN, __VA_ARGS__)
#define VT3_WARN_ONCE(...) do { static int vt3_warned_; if (vt3_warned_++ == 0) VT3_WARN(__VA_ARGS__); } while (0)
#define VT3_ERROR(...) vt3_log(VT3_LOG_ERROR, __VA_ARGS__)

#endif /* VT3_LOG_H */
