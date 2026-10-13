/* winmm subset: timing only. */
#ifndef VT3_WIN32_WINMM_H
#define VT3_WIN32_WINMM_H

#include "win32/types.h"

DWORD    WINAPI timeGetTime(void);
MMRESULT WINAPI timeBeginPeriod(UINT period);
MMRESULT WINAPI timeEndPeriod(UINT period);

/* Multimedia timers. timeSetEvent returns the timer id, or 0 on failure. */
MMRESULT WINAPI timeSetEvent(UINT delay, UINT resolution, LPVOID proc, DWORD user, UINT flags);
MMRESULT WINAPI timeKillEvent(UINT id);
MMRESULT WINAPI timeGetDevCaps(LPVOID caps, UINT size);

#endif /* VT3_WIN32_WINMM_H */
