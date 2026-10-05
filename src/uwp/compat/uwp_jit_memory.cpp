// SPDX-FileCopyrightText: 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// kernel32 VirtualAlloc/VirtualProtect for the dynarmic JIT. Under WINAPI_FAMILY_APP the SDK
// forwards them to the *FromApp variants, which refuse PAGE_EXECUTE_READWRITE. They are resolved
// through GetProcAddress because the forwarders are emitted into every C object that includes
// <memoryapi.h>. This file must not include any Windows headers.

extern "C" {

__declspec(dllimport) void* __stdcall GetModuleHandleW(const wchar_t* module_name);
__declspec(dllimport) void*(__stdcall* GetProcAddress(void* module, const char* proc_name))();

namespace {

using VirtualAllocFn = void*(__stdcall*)(void*, unsigned __int64, unsigned long, unsigned long);
using VirtualProtectFn = int(__stdcall*)(void*, unsigned __int64, unsigned long, unsigned long*);
using VirtualFreeFn = int(__stdcall*)(void*, unsigned __int64, unsigned long);

constexpr unsigned long kMemCommit = 0x1000;
constexpr unsigned long kMemReserve = 0x2000;
constexpr unsigned long kMemRelease = 0x8000;
constexpr unsigned long kPageExecuteReadWrite = 0x40;

VirtualAllocFn g_virtual_alloc = nullptr;
VirtualProtectFn g_virtual_protect = nullptr;
VirtualFreeFn g_virtual_free = nullptr;
bool g_resolved = false;
bool g_rwx_ok = false;

void Resolve() {
    if (g_resolved) {
        return;
    }
    g_resolved = true;
    void* kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) {
        return;
    }
    g_virtual_alloc = reinterpret_cast<VirtualAllocFn>(GetProcAddress(kernel32, "VirtualAlloc"));
    g_virtual_protect =
        reinterpret_cast<VirtualProtectFn>(GetProcAddress(kernel32, "VirtualProtect"));
    g_virtual_free = reinterpret_cast<VirtualFreeFn>(GetProcAddress(kernel32, "VirtualFree"));

    // Probe one RWX page so a refusing container is reported at startup.
    if (g_virtual_alloc && g_virtual_free) {
        if (void* probe =
                g_virtual_alloc(nullptr, 0x1000, kMemCommit | kMemReserve, kPageExecuteReadWrite)) {
            g_rwx_ok = true;
            g_virtual_free(probe, 0, kMemRelease);
        }
    }
}

} // namespace

void* citra_uwp_kernel32_VirtualAlloc(void* address, unsigned __int64 size,
                                      unsigned long allocation_type, unsigned long protect) {
    Resolve();
    if (!g_virtual_alloc) {
        return nullptr;
    }
    return g_virtual_alloc(address, size, allocation_type, protect);
}

int citra_uwp_kernel32_VirtualProtect(void* address, unsigned __int64 size,
                                      unsigned long new_protect, unsigned long* old_protect) {
    Resolve();
    if (!g_virtual_protect) {
        return 0;
    }
    return g_virtual_protect(address, size, new_protect, old_protect);
}

// Non-zero when the kernel32 exports resolved and the container granted an executable page.
int citra_uwp_JitMemorySupported(void) {
    Resolve();
    return g_virtual_alloc && g_virtual_protect && g_rwx_ok;
}

} // extern "C"
