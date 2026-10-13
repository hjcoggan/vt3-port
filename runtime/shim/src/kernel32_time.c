/* Timers. Monotonic clock, so GetTickCount wraps like Win32 at 2^32 ms. */
#include "shim_internal.h"
#include "win32/kernel32.h"
#include "win32/winmm.h"

#include <stdint.h>
#include <time.h>

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t g_start_ns;

static uint64_t since_start_ns(void)
{
    if (g_start_ns == 0) g_start_ns = mono_ns();
    return mono_ns() - g_start_ns;
}

/* Milliseconds since boot, as on Windows. CLOCK_MONOTONIC counts from boot on macOS. */
DWORD WINAPI GetTickCount(void)
{
    return (DWORD)(mono_ns() / 1000000u);
}

DWORD WINAPI timeGetTime(void)
{
    return GetTickCount();
}

MMRESULT WINAPI timeBeginPeriod(UINT period)
{
    (void)period;   /* the monotonic clock has 1 ms resolution already */
    return TIMERR_NOERROR;
}

MMRESULT WINAPI timeEndPeriod(UINT period)
{
    (void)period;
    return TIMERR_NOERROR;
}

BOOL WINAPI QueryPerformanceFrequency(LPLONGLONG frequency)
{
    vt3s_store_u64(frequency, 1000000000u);   /* counts are nanoseconds */
    return TRUE;
}

BOOL WINAPI QueryPerformanceCounter(LPLONGLONG count)
{
    vt3s_store_u64(count, since_start_ns());
    return TRUE;
}

void WINAPI Sleep(DWORD milliseconds)
{
    struct timespec ts = {
        .tv_sec = milliseconds / 1000u,
        .tv_nsec = (long)(milliseconds % 1000u) * 1000000L,
    };
    while (nanosleep(&ts, &ts) != 0) {
        /* interrupted: nanosleep leaves the remainder in ts, so keep sleeping */
    }
}
