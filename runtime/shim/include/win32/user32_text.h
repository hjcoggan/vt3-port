/*
 * Formatted text from user32. The implementation is shim/src/user32_text.c.
 *
 * wsprintfA is cdecl (WINAPIV): the caller pops its arguments. The adapter passes the
 * guest address of the first variadic argument as the last parameter, so the C
 * prototype here has that parameter in place of "...". See tools/recomp/rt/gen_adapters.py.
 */
#ifndef VT3_WIN32_USER32_TEXT_H
#define VT3_WIN32_USER32_TEXT_H

#include "win32/types.h"

#ifndef WINAPIV
#define WINAPIV
#endif

int WINAPIV wsprintfA(LPSTR buf, LPCSTR fmt, GPTR args);
int WINAPI  wvsprintfA(LPSTR buf, LPCSTR fmt, GPTR arglist);

#endif /* VT3_WIN32_USER32_TEXT_H */
