/*
 * Win32 ABI types as the recompiled guest code sees them.
 *
 * Pointers in the guest are 32-bit guest addresses (vt3_gaddr), not host
 * pointers. Each shim function takes its arguments as plain C values. The
 * recompiler marshals them from the x86 stdcall stack, so WINAPI is empty
 * here. HANDLE values are small non-zero ids from the shim's handle table.
 * They are not host pointers.
 *
 * This header never includes <windows.h>. Nothing from the Windows SDK is
 * needed or wanted.
 */
#ifndef VT3_WIN32_TYPES_H
#define VT3_WIN32_TYPES_H

#include "vt3/vt3_guest.h"

#include <stdint.h>

#define WINAPI
#define CALLBACK
#define STDMETHODCALLTYPE

typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t  LONG;
typedef uint32_t UINT;
typedef int32_t  INT;
typedef uint32_t BOOL;
typedef int64_t  LONGLONG;
typedef uint64_t ULONGLONG;
typedef uint32_t SIZE_T;
typedef uint32_t HANDLE;
typedef uint32_t HRESULT;
typedef uint32_t MMRESULT;
typedef char     CHAR;

/* Any guest pointer. Named per use so the prototypes read like the SDK ones. */
typedef vt3_gaddr GPTR;
typedef GPTR LPVOID, LPCVOID, LPDWORD, LPLONG, LPLONGLONG, LPCSTR, LPSTR;
typedef GPTR LPSECURITY_ATTRIBUTES, LPOVERLAPPED, LPCGUID, LPUNKNOWN;
typedef GPTR PVOID;

#define TRUE  1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)0xFFFFFFFFu)

#define HEAP_NO_SERIALIZE        0x00000001u
#define HEAP_GENERATE_EXCEPTIONS 0x00000004u
#define HEAP_ZERO_MEMORY         0x00000008u

#define GENERIC_READ             0x80000000u
#define GENERIC_WRITE            0x40000000u
#define FILE_SHARE_READ          0x00000001u
#define OPEN_EXISTING            3u
#define FILE_ATTRIBUTE_NORMAL    0x00000080u
#define FILE_BEGIN               0u
#define FILE_CURRENT             1u
#define FILE_END                 2u
#define INVALID_SET_FILE_POINTER 0xFFFFFFFFu

#define ERROR_SUCCESS               0u
#define ERROR_FILE_NOT_FOUND        2u
#define ERROR_ACCESS_DENIED         5u
#define ERROR_INVALID_HANDLE        6u
#define ERROR_NOT_ENOUGH_MEMORY     8u
#define ERROR_INVALID_PARAMETER     87u
#define ERROR_CALL_NOT_IMPLEMENTED  120u

#define TIMERR_NOERROR 0u

#define S_OK         0u
#define E_FAIL       0x80004005u
#define E_NOTIMPL    0x80004001u

#endif /* VT3_WIN32_TYPES_H */
