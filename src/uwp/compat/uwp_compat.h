/*
 * Copyright Citra Emulator Project / Azahar Emulator Project
 * Licensed under GPLv2 or any later version
 * Refer to the license.txt file included.
 *
 * UWP (WindowsStore) Win32 compatibility shim: re-declares Win32 entry points hidden from the
 * app partition as wrappers over their app-container replacements. Force-included (/FI) into C
 * and C++ targets, so it must be self-contained, valid C and header-only.
 */

#ifndef CITRA_UWP_COMPAT_H
#define CITRA_UWP_COMPAT_H

#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

/* windows.h must precede memoryapi.h or winnt.h fails with "No Target Architecture". */
/* clang-format off */
#include <windows.h>
#include <memoryapi.h>
/* clang-format on */

/* Longest path widened on the stack; no in-tree caller approaches Windows' 32767 limit. */
#define CITRA_UWP_MAX_WIDE 2048

/* Split dwFlagsAndAttributes into the three fields CreateFile2 expects. */
static __inline void citra_uwp_split_file_flags(DWORD combined,
                                                CREATEFILE2_EXTENDED_PARAMETERS* ep) {
    ep->dwFileAttributes = combined & 0x0000FFFFu;   /* FILE_ATTRIBUTE_* */
    ep->dwSecurityQosFlags = combined & 0x000F0000u; /* SECURITY_*       */
    ep->dwFileFlags = combined & 0xFFF00000u;        /* FILE_FLAG_*      */
}

/* Widen an ACP string into caller-provided storage. Returns 0 on failure. */
static __inline int citra_uwp_widen(LPCSTR narrow, WCHAR* out, int out_chars) {
    if (narrow == NULL) {
        return 0;
    }
    return MultiByteToWideChar(CP_ACP, 0, narrow, -1, out, out_chars) > 0;
}

static __inline HANDLE WINAPI citra_uwp_CreateFileW(LPCWSTR file_name, DWORD desired_access,
                                                    DWORD share_mode,
                                                    LPSECURITY_ATTRIBUTES security_attributes,
                                                    DWORD creation_disposition,
                                                    DWORD flags_and_attributes,
                                                    HANDLE template_file) {
    CREATEFILE2_EXTENDED_PARAMETERS ep;
    ZeroMemory(&ep, sizeof(ep));
    ep.dwSize = sizeof(ep);
    citra_uwp_split_file_flags(flags_and_attributes, &ep);
    ep.lpSecurityAttributes = security_attributes;
    ep.hTemplateFile = template_file;
    return CreateFile2(file_name, desired_access, share_mode, creation_disposition, &ep);
}

static __inline HANDLE WINAPI citra_uwp_CreateFileA(LPCSTR file_name, DWORD desired_access,
                                                    DWORD share_mode,
                                                    LPSECURITY_ATTRIBUTES security_attributes,
                                                    DWORD creation_disposition,
                                                    DWORD flags_and_attributes,
                                                    HANDLE template_file) {
    WCHAR wide[CITRA_UWP_MAX_WIDE];
    if (!citra_uwp_widen(file_name, wide, CITRA_UWP_MAX_WIDE)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    return citra_uwp_CreateFileW(wide, desired_access, share_mode, security_attributes,
                                 creation_disposition, flags_and_attributes, template_file);
}

static __inline DWORD WINAPI citra_uwp_GetFileSize(HANDLE file, LPDWORD file_size_high) {
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) {
        return INVALID_FILE_SIZE;
    }
    if (file_size_high != NULL) {
        *file_size_high = (DWORD)size.HighPart;
    }
    return (DWORD)size.LowPart;
}

static __inline HANDLE WINAPI citra_uwp_CreateFileMappingW(HANDLE file,
                                                           LPSECURITY_ATTRIBUTES attributes,
                                                           DWORD protect, DWORD max_size_high,
                                                           DWORD max_size_low, LPCWSTR name) {
    ULONG64 maximum_size = ((ULONG64)max_size_high << 32) | (ULONG64)max_size_low;
    return CreateFileMappingFromApp(file, attributes, protect, maximum_size, name);
}

static __inline HANDLE WINAPI citra_uwp_CreateFileMappingA(HANDLE file,
                                                           LPSECURITY_ATTRIBUTES attributes,
                                                           DWORD protect, DWORD max_size_high,
                                                           DWORD max_size_low, LPCSTR name) {
    WCHAR wide[CITRA_UWP_MAX_WIDE];
    if (name == NULL) {
        return citra_uwp_CreateFileMappingW(file, attributes, protect, max_size_high, max_size_low,
                                            NULL);
    }
    if (!citra_uwp_widen(name, wide, CITRA_UWP_MAX_WIDE)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    return citra_uwp_CreateFileMappingW(file, attributes, protect, max_size_high, max_size_low,
                                        wide);
}

static __inline LPVOID WINAPI citra_uwp_MapViewOfFile(HANDLE mapping, DWORD desired_access,
                                                      DWORD offset_high, DWORD offset_low,
                                                      SIZE_T number_of_bytes) {
    ULONG64 offset = ((ULONG64)offset_high << 32) | (ULONG64)offset_low;
    return MapViewOfFileFromApp(mapping, desired_access, offset, number_of_bytes);
}

/* MapViewOfFileFromApp cannot map at a fixed address; every in-tree caller passes NULL. */
static __inline LPVOID WINAPI citra_uwp_MapViewOfFileEx(HANDLE mapping, DWORD desired_access,
                                                        DWORD offset_high, DWORD offset_low,
                                                        SIZE_T number_of_bytes,
                                                        LPVOID base_address) {
    (void)base_address;
    return citra_uwp_MapViewOfFile(mapping, desired_access, offset_high, offset_low,
                                   number_of_bytes);
}

/* The *FromApp variants reject PAGE_EXECUTE_READWRITE, so the JIT routes to the kernel32
 * exports resolved in uwp_jit_memory.cpp. */

#ifdef __cplusplus
extern "C" {
#endif

LPVOID citra_uwp_kernel32_VirtualAlloc(LPVOID address, unsigned __int64 size,
                                       unsigned long allocation_type, unsigned long protect);
int citra_uwp_kernel32_VirtualProtect(LPVOID address, unsigned __int64 size,
                                      unsigned long new_protect, unsigned long* old_protect);

#ifdef __cplusplus
}
#endif

static __inline LPVOID WINAPI citra_uwp_VirtualAlloc(LPVOID address, SIZE_T size,
                                                     DWORD allocation_type, DWORD protect) {
    return citra_uwp_kernel32_VirtualAlloc(address, size, allocation_type, protect);
}

static __inline BOOL WINAPI citra_uwp_VirtualProtect(LPVOID address, SIZE_T size, DWORD new_protect,
                                                     PDWORD old_protect) {
    return citra_uwp_kernel32_VirtualProtect(address, size, new_protect, old_protect);
}

/* The SDK does not declare these under WINAPI_FAMILY_APP, so third-party calls land here. */
#define CreateFileW citra_uwp_CreateFileW
#define CreateFileA citra_uwp_CreateFileA
#define GetFileSize citra_uwp_GetFileSize
#define CreateFileMappingW citra_uwp_CreateFileMappingW
#define CreateFileMappingA citra_uwp_CreateFileMappingA
#define MapViewOfFile citra_uwp_MapViewOfFile
#define MapViewOfFileEx citra_uwp_MapViewOfFileEx
#define VirtualAlloc citra_uwp_VirtualAlloc
#define VirtualProtect citra_uwp_VirtualProtect

/* Prevent the A/W macros from renaming identically named Azahar methods. */
#undef CreateDirectory
#undef DeleteFile

#endif /* WINAPI_FAMILY_APP */

#endif /* CITRA_UWP_COMPAT_H */
