// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <fmt/format.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "common/string_util.h"
#include "dynamic_library.h"

namespace Common {

DynamicLibrary::DynamicLibrary() = default;

DynamicLibrary::DynamicLibrary(void* handle_) : handle{handle_} {}

DynamicLibrary::DynamicLibrary(std::string_view name, int major, int minor) {
    auto full_name = GetLibraryName(name, major, minor);
    void(Load(full_name));
}

DynamicLibrary::~DynamicLibrary() {
    if (handle) {
#if defined(_WIN32)
        FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
        dlclose(handle);
#endif // defined(_WIN32)
        handle = nullptr;
    }
}

bool DynamicLibrary::Load(std::string_view filename) {
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    // LoadPackagedLibrary accepts only a packaged module's file name, not a path.
    const std::string_view name = [filename] {
        const auto slash = filename.find_last_of("\\/");
        return slash == std::string_view::npos ? filename : filename.substr(slash + 1);
    }();
    const std::wstring wide_name = Common::UTF8ToUTF16W(std::string{name});

    handle = reinterpret_cast<void*>(LoadPackagedLibrary(wide_name.c_str(), 0));
    if (!handle) {
        load_error = "LoadPackagedLibrary(" + std::string{name} +
                     ") failed: " + std::to_string(GetLastError()) +
                     " (the library must be packaged alongside the executable)";
        return false;
    }
#elif defined(_WIN32)
    handle = reinterpret_cast<void*>(LoadLibraryA(filename.data()));
    if (!handle) {
        DWORD error_message_id = GetLastError();
        LPSTR message_buffer = nullptr;
        std::size_t size =
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                               FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, error_message_id, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                           reinterpret_cast<LPSTR>(&message_buffer), 0, nullptr);
        std::string message(message_buffer, size);
        load_error = message;
        return false;
    }
#else
    handle = dlopen(filename.data(), RTLD_LAZY);
    if (!handle) {
        load_error = dlerror();
        return false;
    }
#endif // defined(_WIN32)
    return true;
}

void* DynamicLibrary::GetRawSymbol(std::string_view name) const {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name.data()));
#else
    return dlsym(handle, name.data());
#endif // defined(_WIN32)
}

std::string DynamicLibrary::GetLibraryName(std::string_view name, int major, int minor) {
#if defined(_WIN32)
    if (major >= 0 && minor >= 0) {
        return fmt::format("{}-{}-{}.dll", name, major, minor);
    } else if (major >= 0) {
        return fmt::format("{}-{}.dll", name, major);
    } else {
        return fmt::format("{}.dll", name);
    }
#elif defined(__APPLE__)
    auto prefix = name.starts_with("lib") ? "" : "lib";
    if (major >= 0 && minor >= 0) {
        return fmt::format("{}{}.{}.{}.dylib", prefix, name, major, minor);
    } else if (major >= 0) {
        return fmt::format("{}{}.{}.dylib", prefix, name, major);
    } else {
        return fmt::format("{}{}.dylib", prefix, name);
    }
#else
    auto prefix = name.starts_with("lib") ? "" : "lib";
    if (major >= 0 && minor >= 0) {
        return fmt::format("{}{}.so.{}.{}", prefix, name, major, minor);
    } else if (major >= 0) {
        return fmt::format("{}{}.so.{}", prefix, name, major);
    } else {
        return fmt::format("{}{}.so", prefix, name);
    }
#endif
}

} // namespace Common
