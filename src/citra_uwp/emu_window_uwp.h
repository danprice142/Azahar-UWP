// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>

#include "citra_uwp/uwp_gl.h"
#include "core/frontend/emu_window.h"

struct SDL_Window;
union SDL_Event;
struct _SDL_GameController;
using SDL_GameController = _SDL_GameController;

namespace Core {
class System;
}

namespace UWP {

class ImGuiManager;

/**
 * EmuWindow backed by an SDL2 window with an OpenGL context from mesa-uwp. mesa locates the
 * CoreWindow itself, so the GL context must be created on the thread that owns it.
 */
class EmuWindow_UWP : public Frontend::EmuWindow {
public:
    explicit EmuWindow_UWP(Core::System& system, bool is_secondary = false);
    ~EmuWindow_UWP() override;

    EmuWindow_UWP(const EmuWindow_UWP&) = delete;
    EmuWindow_UWP& operator=(const EmuWindow_UWP&) = delete;

    bool IsValid() const {
        return is_valid;
    }

    void RequestQuit() {
        should_quit = true;
    }

    bool ShouldQuit() const {
        return should_quit;
    }

    const std::string& DescribeGL() const {
        return GetGLDescription();
    }

    void UpdateLayout() {
        OnResize();
    }

    /// True when a window-size or lifecycle change needs UpdateLayout(), which the main loop
    /// calls after synchronizing the emulation worker.
    bool TakeLayoutUpdateRequest() {
        return layout_update_requested.exchange(false, std::memory_order_acq_rel);
    }

    SDL_Window* GetSDLWindow() const {
        return sdl_window;
    }

    // Frontend::EmuWindow
    void PollEvents() override;

    /// PollEvents variant that lets the UI consume events first.
    void PollEvents(ImGuiManager* imgui);
    void UpdateVirtualTouchCursor(bool gameplay_input_enabled);
    Frontend::EmuWindow::CursorInfo GetCursorInfo() const override;
    /// Mouse position in window coordinates while it was recently used during gameplay.
    bool GetMouseCursor(float& x, float& y) const;
    std::unique_ptr<Frontend::GraphicsContext> CreateSharedContext() const override;
    void SaveContext() override;
    void RestoreContext() override;

    // Frontend::GraphicsContext
    void MakeCurrent() override;
    void DoneCurrent() override;
    void SwapBuffers() override;
    bool UsesSingleContextForOpenGLPresentation() const override {
        return presentation_uses_render_context.load(std::memory_order_relaxed);
    }
    bool UsesVSyncForFramePacing() const override {
        // Presentation VSync paces only the UI thread once emulation runs on a worker context.
        return UsesSingleContextForOpenGLPresentation() &&
               vsync_paces_emulation.load(std::memory_order_relaxed);
    }

    /// Selects the mailbox synchronization policy when emulation is moved onto a shared context.
    void SetOpenGLPresentationUsesRenderContext(bool value) {
        presentation_uses_render_context.store(value, std::memory_order_relaxed);
    }

    /// Oldest path delivered through SDL_DROPFILE (WinRT file activation), if any.
    std::optional<std::string> TakeDroppedFile();

    /// Lifecycle edges observed by PollEvents; the main loop applies the background policy.
    bool TakeEnteredBackground();
    bool TakeEnteredForeground();

private:
    struct DisplayModeWatcher;

    void OnResize();
    void UpdateSwapInterval();
    bool InitializeDisplayModeWatcher();
    void ShutdownDisplayModeWatcher();
    bool RefreshHostSurfaceSize(bool request_layout);
    SDL_GameController* FindCursorGamepad();
    void ReleaseVirtualTouch();
    void OnMouseEvent(const SDL_Event& event);
    void ReleaseLocalInput();

    Core::System& system;
    SDL_Window* sdl_window = nullptr;
    GLContext gl;
    HDC saved_dc = nullptr;
    HGLRC saved_context = nullptr;
    std::deque<std::string> dropped_files;
    bool is_valid = false;
    bool should_quit = false;
    bool entered_background = false;
    bool entered_foreground = false;
    int applied_swap_interval = -1;
    std::shared_ptr<DisplayModeWatcher> display_mode_watcher;
    // Width in the high 32 bits, height in the low 32 bits, so a callback cannot tear the pair.
    std::atomic<std::uint64_t> cached_host_surface_size{0};
    std::atomic_bool layout_update_requested{false};
    std::atomic_bool presentation_uses_render_context{true};
    std::atomic_bool vsync_paces_emulation{false};

    SDL_GameController* cursor_gamepad = nullptr;
    // Normalized touchscreen coordinates; the rendered bottom-screen rectangle varies by layout.
    float virtual_cursor_x = 0.5f;
    float virtual_cursor_y = 0.5f;
    bool virtual_cursor_initialized = false;
    bool virtual_cursor_pressed = false;
    bool virtual_cursor_visible = false;
    u64 remote_mouse_sequence = 0;
    std::chrono::steady_clock::time_point virtual_cursor_last_update{};
    std::chrono::steady_clock::time_point virtual_cursor_last_moved{};

    bool local_input_enabled = false;
    bool mouse_touch_pressed = false;
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    std::chrono::steady_clock::time_point mouse_last_used{};
};

} // namespace UWP
