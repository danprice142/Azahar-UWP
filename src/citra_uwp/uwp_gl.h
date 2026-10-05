// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>

#include <windows.h>

// OpenGL on Xbox/UWP is provided by mesa-uwp's d3d12 driver. The app partition has no
// user32/gdi32, so libgallium_wgl.dll exports the windowing entry points (GetDC/WindowFromDC
// are plain casts) and opengl32.dll exports the wgl* entry points. mesa resolves the CoreWindow
// itself, so contexts must be created on the thread that owns it. Exports are undecorated.

extern "C" {

HWND WINAPI CreateWindowEx(DWORD ex_style, LPCSTR class_name, LPCSTR window_name, DWORD style,
                           int x, int y, int width, int height, HWND parent, HMENU menu,
                           HINSTANCE instance, LPVOID param);
BOOL WINAPI DestroyWindow(HWND window);
HDC WINAPI GetDC(HWND window);
int WINAPI ReleaseDC(HWND window, HDC dc);
BOOL WINAPI GetClientRect(HWND window, LPRECT rect);
HWND WINAPI WindowFromDC(HDC dc);

// The unprefixed GDI names (ChoosePixelFormat/SetPixelFormat) are not exported.
int WINAPI wglChoosePixelFormat(HDC dc, const PIXELFORMATDESCRIPTOR* pfd);
BOOL WINAPI wglSetPixelFormat(HDC dc, int format, const PIXELFORMATDESCRIPTOR* pfd);
int WINAPI wglDescribePixelFormat(HDC dc, int format, UINT bytes, PIXELFORMATDESCRIPTOR* pfd);
HGLRC WINAPI wglCreateContext(HDC dc);
BOOL WINAPI wglDeleteContext(HGLRC context);
BOOL WINAPI wglMakeCurrent(HDC dc, HGLRC context);
BOOL WINAPI wglShareLists(HGLRC to, HGLRC from);
BOOL WINAPI wglSwapBuffers(HDC dc);
PROC WINAPI wglGetProcAddress(LPCSTR name);
HGLRC WINAPI wglGetCurrentContext(void);
HDC WINAPI wglGetCurrentDC(void);

} // extern "C"

namespace UWP {

/// A GL context plus the pseudo-window/DC pair mesa hands back for it.
struct GLContext {
    HWND window = nullptr;
    HDC dc = nullptr;
    HGLRC context = nullptr;
    /// False when `window` is a synthetic token that must not be passed to DestroyWindow/ReleaseDC.
    bool owns_window = false;

    explicit operator bool() const {
        return context != nullptr;
    }
};

/// Runs the WGL bootstrap and returns the context, left not current. `share_with` is the primary
/// context whose drawable and objects are shared; a falsy GLContext signals failure.
GLContext CreateGLContext(const GLContext* share_with = nullptr);

void DestroyGLContext(GLContext& ctx);

/// Loads GL entry points into glad; a context must be current.
bool LoadGLFunctions();

/// Sets the CoreWindow swap-chain size; must precede CreateGLContext (Series S is capped at 1440p).
bool SetMesaUwpOutputSize(unsigned width, unsigned height);

/// Sets the WGL swap interval; must be disabled while the core's own frame limiter is active.
bool SetGLSwapInterval(int interval);

/// "vendor / renderer / version" captured during LoadGLFunctions; does not touch GL.
const std::string& GetGLDescription();

} // namespace UWP
