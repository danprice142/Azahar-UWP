// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include <cstdint>
#include <limits>

#include <glad/glad.h>

#include "citra_uwp/uwp_gl.h"
#include "common/logging/log.h"

namespace UWP {

namespace {

std::string g_gl_description = "<GL not initialised>";

/// Base for synthetic HWNDs used when mesa's CreateWindowEx declines to hand one out; mesa only
/// uses the value as a drawable-map key.
constexpr std::uintptr_t kSyntheticWindowTokenBase = 0xA2A4A2;

/// wglGetProcAddress reports failure with several sentinel values, not just NULL.
bool IsInvalidProcAddress(PROC proc) {
    return proc == nullptr || proc == reinterpret_cast<PROC>(1) ||
           proc == reinterpret_cast<PROC>(2) || proc == reinterpret_cast<PROC>(3) ||
           proc == reinterpret_cast<PROC>(-1);
}

/// GL 1.1 entry points come from opengl32's export table, not wglGetProcAddress.
void* GLADLoader(const char* name) {
    if (const PROC proc = wglGetProcAddress(name); !IsInvalidProcAddress(proc)) {
        return reinterpret_cast<void*>(proc);
    }

    // GetModuleHandleW is callable from an app container, whereas LoadLibraryW is not.
    static HMODULE opengl32 = ::GetModuleHandleW(L"opengl32.dll");
    if (opengl32 == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void*>(::GetProcAddress(opengl32, name));
}

PIXELFORMATDESCRIPTOR MakePixelFormatDescriptor() {
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    return pfd;
}

} // namespace

GLContext CreateGLContext(const GLContext* share_with) {
    GLContext ctx;

    if (share_with != nullptr) {
        if (!*share_with || share_with->dc == nullptr) {
            LOG_ERROR(Frontend, "Cannot create shared GL context from an invalid primary context");
            return {};
        }

        // A pixel format is assigned to a drawable only once, so shared contexts reuse the
        // primary DC.
        ctx.window = share_with->window;
        ctx.dc = share_with->dc;
    } else {
        // mesa's HWND is an opaque key mapped to a framebuffer; presentation goes to the
        // CoreWindow.
        ctx.window =
            CreateWindowEx(0, nullptr, nullptr, 0, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
        if (ctx.window != nullptr) {
            ctx.owns_window = true;
            ctx.dc = GetDC(ctx.window);
        } else {
            static std::atomic<std::uintptr_t> next_synthetic_token{kSyntheticWindowTokenBase};
            const std::uintptr_t token =
                next_synthetic_token.fetch_add(1, std::memory_order_relaxed);
            LOG_INFO(
                Frontend,
                "mesa's CreateWindowEx returned null (err {}); using synthetic window token {:x}",
                ::GetLastError(), token);
            ctx.window = reinterpret_cast<HWND>(token);
            ctx.dc = reinterpret_cast<HDC>(ctx.window);
        }
    }

    if (ctx.dc == nullptr) {
        LOG_CRITICAL(Frontend, "GetDC failed (mesa GDI shim)");
        DestroyGLContext(ctx);
        return {};
    }

    int pixel_format = 0;
    if (share_with == nullptr) {
        const PIXELFORMATDESCRIPTOR pfd = MakePixelFormatDescriptor();
        pixel_format = wglChoosePixelFormat(ctx.dc, &pfd);
        if (pixel_format == 0) {
            LOG_CRITICAL(Frontend, "wglChoosePixelFormat found no usable format: {}",
                         ::GetLastError());
            DestroyGLContext(ctx);
            return {};
        }

        if (!wglSetPixelFormat(ctx.dc, pixel_format, &pfd)) {
            LOG_CRITICAL(Frontend, "wglSetPixelFormat({}) failed: {}", pixel_format,
                         ::GetLastError());
            DestroyGLContext(ctx);
            return {};
        }
    }

    ctx.context = wglCreateContext(ctx.dc);
    if (ctx.context == nullptr) {
        LOG_CRITICAL(Frontend, "wglCreateContext failed: {}", ::GetLastError());
        DestroyGLContext(ctx);
        return {};
    }

    if (share_with != nullptr && !wglShareLists(share_with->context, ctx.context)) {
        // Treat as a failure; the renderer's background context would be useless otherwise.
        LOG_CRITICAL(Frontend, "wglShareLists failed: {}", ::GetLastError());
        DestroyGLContext(ctx);
        return {};
    }

    if (share_with != nullptr) {
        LOG_INFO(Frontend, "Created shared GL context on primary drawable");
    } else {
        LOG_INFO(Frontend, "Created GL context (pixel format {})", pixel_format);
    }
    return ctx;
}

void DestroyGLContext(GLContext& ctx) {
    if (ctx.context != nullptr) {
        if (wglGetCurrentContext() == ctx.context) {
            wglMakeCurrent(nullptr, nullptr);
        }
        wglDeleteContext(ctx.context);
        ctx.context = nullptr;
    }
    // Only release a window mesa created; the synthetic token is not a real window.
    if (ctx.owns_window) {
        if (ctx.dc != nullptr) {
            ReleaseDC(ctx.window, ctx.dc);
        }
        DestroyWindow(ctx.window);
        ctx.owns_window = false;
    }
    ctx.dc = nullptr;
    ctx.window = nullptr;
}

bool LoadGLFunctions() {
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(GLADLoader))) {
        LOG_CRITICAL(Frontend, "gladLoadGLLoader failed -- no usable OpenGL entry points");
        return false;
    }

    const auto gl_string = [](GLenum name) -> const char* {
        const auto* value = reinterpret_cast<const char*>(glGetString(name));
        return value != nullptr ? value : "<null>";
    };
    g_gl_description = std::string{gl_string(GL_VENDOR)} + " / " + gl_string(GL_RENDERER) + " / " +
                       gl_string(GL_VERSION);
    LOG_INFO(Frontend, "GL_VENDOR:   {}", gl_string(GL_VENDOR));
    LOG_INFO(Frontend, "GL_RENDERER: {}", gl_string(GL_RENDERER));
    LOG_INFO(Frontend, "GL_VERSION:  {}", gl_string(GL_VERSION));

    // The renderer requires GL 4.3; report clearly rather than failing inside the renderer.
    GLint major = 0;
    GLint minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major < 4 || (major == 4 && minor < 3)) {
        LOG_CRITICAL(Frontend, "OpenGL {}.{} is below the 4.3 the renderer requires", major, minor);
        return false;
    }

    return true;
}

bool SetMesaUwpOutputSize(unsigned width, unsigned height) {
    if (width == 0 || height == 0 ||
        width > static_cast<unsigned>(std::numeric_limits<int>::max()) ||
        height > static_cast<unsigned>(std::numeric_limits<int>::max())) {
        LOG_ERROR(Frontend, "Invalid mesa UWP output size {}x{}", width, height);
        return false;
    }

    // Resolve the Azahar extension dynamically so mesa's import library stays untouched.
    static const auto set_output_size = [] {
        const HMODULE mesa = ::GetModuleHandleW(L"libgallium_wgl.dll");
        if (mesa == nullptr) {
            return static_cast<void(__cdecl*)(int, int)>(nullptr);
        }
        return reinterpret_cast<void(__cdecl*)(int, int)>(
            ::GetProcAddress(mesa, "uwp_set_output_size"));
    }();
    if (set_output_size == nullptr) {
        LOG_CRITICAL(Frontend, "The packaged mesa UWP runtime does not expose uwp_set_output_size");
        return false;
    }

    set_output_size(static_cast<int>(width), static_cast<int>(height));
    return true;
}

bool SetGLSwapInterval(int interval) {
    using WglSwapIntervalExt = BOOL(WINAPI*)(int);
    using WglGetSwapIntervalExt = int(WINAPI*)();

    const PROC set_proc = wglGetProcAddress("wglSwapIntervalEXT");
    if (IsInvalidProcAddress(set_proc)) {
        LOG_WARNING(Frontend, "WGL_EXT_swap_control is unavailable; swap interval is unchanged");
        return false;
    }

    const auto set_swap_interval = reinterpret_cast<WglSwapIntervalExt>(set_proc);
    if (!set_swap_interval(interval)) {
        LOG_WARNING(Frontend, "wglSwapIntervalEXT({}) failed: {}", interval, ::GetLastError());
        return false;
    }

    int reported_interval = interval;
    const PROC get_proc = wglGetProcAddress("wglGetSwapIntervalEXT");
    if (!IsInvalidProcAddress(get_proc)) {
        reported_interval = reinterpret_cast<WglGetSwapIntervalExt>(get_proc)();
        if (reported_interval != interval) {
            LOG_WARNING(Frontend,
                        "WGL swap interval request {} was accepted but the driver reports {}; "
                        "falling back to emulator pacing",
                        interval, reported_interval);
            return false;
        }
    }
    LOG_INFO(Frontend, "WGL swap interval set to {} (driver reports {})", interval,
             reported_interval);
    return true;
}

const std::string& GetGLDescription() {
    return g_gl_description;
}

} // namespace UWP
