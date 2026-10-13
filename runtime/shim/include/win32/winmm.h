/* winmm subset: timing only. */
#ifndef VT3_WIN32_WINMM_H
#define VT3_WIN32_WINMM_H

#include "win32/types.h"

DWORD    WINAPI timeGetTime(void);
MMRESULT WINAPI timeBeginPeriod(UINT period);
MMRESULT WINAPI timeEndPeriod(UINT period);

#endif /* VT3_WIN32_WINMM_H */
