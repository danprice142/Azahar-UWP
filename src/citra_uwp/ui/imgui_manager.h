// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include "common/common_types.h"

struct SDL_Window;
struct _SDL_GameController;
using SDL_GameController = _SDL_GameController;
union SDL_Event;

namespace UWP {

/// Owns the ImGui context, rendering backends, and controller navigation state.
class ImGuiManager {
public:
    ImGuiManager() = default;
    ~ImGuiManager();

    ImGuiManager(const ImGuiManager&) = delete;
    ImGuiManager& operator=(const ImGuiManager&) = delete;

    bool Initialize(SDL_Window* window);

    /// Returns true when ImGui consumes the SDL event.
    bool ProcessEvent(const SDL_Event& event);

    void BeginFrame(unsigned framebuffer_width, unsigned framebuffer_height);
    /// Routes DSU mouse input to ImGui while the frontend UI is active.
    void SetRemoteMouseForFrontend(bool enabled) {
        remote_mouse_for_frontend = enabled;
    }

    void FocusMainMenuBar();

    /// Restores controller navigation focus when no item holds it.
    void EnsureNavFocus();

    void EndFrame();

    bool IsInitialized() const {
        return initialized;
    }

    /// True on the frame Start requests the frontend menu.
    bool TakeMenuFocusRequest() {
        const bool requested = menu_focus_request;
        menu_focus_request = false;
        return requested;
    }

    /// Returns -1 for the previous tab, +1 for the next, or 0 for no request.
    int TakeNavCycleRequest() {
        const int request = nav_cycle_request;
        nav_cycle_request = 0;
        return request;
    }

    bool IsLeftStickPressed();

private:
    void ApplyStyle(float scale);

    void HandleConsoleNavigation();

    void ApplyLeftStickAsDpad();

    void RefreshGamepad();

    SDL_Window* sdl_window = nullptr;
    bool initialized = false;

    SDL_GameController* gamepad = nullptr;

    /// Auto-repeat state for one stick direction.
    struct StickAxis {
        bool held = false;
        float timer = 0.0f;
    };
    StickAxis stick_left;
    StickAxis stick_right;
    StickAxis stick_up;
    StickAxis stick_down;

    bool menu_focus_request = false;
    int nav_cycle_request = 0;
    unsigned long long remote_mouse_sequence = 0;
    bool remote_mouse_active = false;
    bool remote_mouse_for_frontend = true;
    unsigned long long remote_keyboard_sequence = 0;
    std::array<u32, 11> remote_keyboard_bitmap{};
};

} // namespace UWP
