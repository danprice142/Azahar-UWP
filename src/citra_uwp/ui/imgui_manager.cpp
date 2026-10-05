// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>

#include <SDL.h>

// Must precede imgui.h, not just imgui_internal.h -- imgui.h latches the setting.
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
// EnsureNavFocus needs the internal API to find the focused window.
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>
#include <imgui_internal.h>

#include "citra_uwp/ui/imgui_manager.h"
#include "citra_uwp/ui/widgets.h"
#include "citra_uwp/uwp_settings.h"
#include "common/logging/log.h"
#include "input_common/udp/udp.h"

namespace UWP {

namespace {

/// mesa's d3d12 driver provides a 4.6 compatibility profile, so the backend's 1.30 path works.
constexpr const char* kGlslVersion = "#version 130";

/// ImGui's SDL2 backend only toggles SDL text input on Android; WinRT needs it for the InputPane.
void SetPlatformImeData(ImGuiContext*, ImGuiViewport* viewport, ImGuiPlatformImeData* data) {
    const bool wants_text = data->WantVisible || data->WantTextInput;
    if (!wants_text) {
        if (SDL_IsTextInputActive()) {
            SDL_StopTextInput();
        }
        return;
    }

    SDL_Rect input_rect{};
    input_rect.x = static_cast<int>(data->InputPos.x - viewport->Pos.x);
    input_rect.y = static_cast<int>(data->InputPos.y - viewport->Pos.y);
    input_rect.w = 1;
    input_rect.h = std::max(1, static_cast<int>(data->InputLineHeight));
    SDL_SetTextInputRect(&input_rect);

    // Called on every change so the WinRT InputPane updates its caret rectangle.
    SDL_StartTextInput();
}

/// Editing keys the on-screen keyboard delivers as key events rather than SDL_TEXTINPUT.
bool IsOskEditingKey(SDL_Keycode key) {
    switch (key) {
    case SDLK_BACKSPACE:
    case SDLK_DELETE:
    case SDLK_LEFT:
    case SDLK_RIGHT:
    case SDLK_UP:
    case SDLK_DOWN:
    case SDLK_HOME:
    case SDLK_END:
    case SDLK_PAGEUP:
    case SDLK_PAGEDOWN:
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        return true;
    default:
        return false;
    }
}

} // namespace

ImGuiManager::~ImGuiManager() {
    if (!initialized) {
        return;
    }
    if (SDL_IsTextInputActive()) {
        SDL_StopTextInput();
    }
    if (gamepad != nullptr) {
        SDL_GameControllerClose(gamepad);
        gamepad = nullptr;
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    initialized = false;
}

bool ImGuiManager::Initialize(SDL_Window* window) {
    if (window == nullptr) {
        LOG_CRITICAL(Frontend, "ImGuiManager::Initialize called without a window");
        return false;
    }
    sdl_window = window;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    // Segoe UI, falling back to ImGui's embedded font if the file is unavailable.
    ImFontConfig font_config;
    font_config.Flags |= ImFontFlags_NoLoadError;
    static constexpr ImWchar font_ranges[]{0x0020, 0x00FF, 0};
    if (ImFont* segoe_ui = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f,
                                                        &font_config, font_ranges)) {
        io.FontDefault = segoe_ui;
    } else {
        LOG_WARNING(Frontend, "Segoe UI was unavailable; using ImGui's embedded font");
    }
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NoMouse |
                      ImGuiConfigFlags_NoMouseCursorChange;
    // Park the mouse cursor on the focused item so hover behaviour follows navigation. Not
    // NavEnableSetMousePos, which warps the system cursor every frame.
    io.ConfigNavMoveSetMousePos = true;
    // The system cursor is not visible over an exclusive fullscreen surface.
    io.MouseDrawCursor = false;
    // The executable directory is read-only in an app container.
    io.IniFilename = nullptr;

    const float ui_scale = std::clamp(UWP::values.ui_scale.GetValue(), 1.0f, 4.0f);
    ApplyStyle(ui_scale);

    // SDL does not own the GL context; the backend only needs it for multi-viewport support.
    if (!ImGui_ImplSDL2_InitForOpenGL(sdl_window, nullptr)) {
        LOG_CRITICAL(Frontend, "ImGui_ImplSDL2_InitForOpenGL failed");
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init(kGlslVersion)) {
        LOG_CRITICAL(Frontend, "ImGui_ImplOpenGL3_Init failed");
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    // SDL enables text events by default; the IME callback manages the active-field lifetime.
    if (SDL_IsTextInputActive()) {
        SDL_StopTextInput();
    }
    ImGui::GetPlatformIO().Platform_SetImeDataFn = SetPlatformImeData;

    initialized = true;
    LOG_INFO(Frontend, "ImGui {} initialised (scale {}x)", IMGUI_VERSION, ui_scale);
    return true;
}

void ImGuiManager::ApplyStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(9.0f, 7.0f);

    // ImGui's stock dark theme uses blue hover/active states.
    const ImVec4 control{0.10f, 0.10f, 0.11f, 1.00f};
    const ImVec4 control_hovered{0.17f, 0.17f, 0.19f, 1.00f};
    const ImVec4 control_active{0.24f, 0.24f, 0.27f, 1.00f};
    const ImVec4 accent{0.72f, 0.72f, 0.75f, 1.00f};
    style.Colors[ImGuiCol_TitleBg] = style.Colors[ImGuiCol_WindowBg];
    style.Colors[ImGuiCol_TitleBgActive] = style.Colors[ImGuiCol_WindowBg];
    style.Colors[ImGuiCol_TitleBgCollapsed] = style.Colors[ImGuiCol_WindowBg];
    style.Colors[ImGuiCol_FrameBg] = control;
    style.Colors[ImGuiCol_FrameBgHovered] = control_hovered;
    style.Colors[ImGuiCol_FrameBgActive] = control_active;
    style.Colors[ImGuiCol_Button] = control;
    style.Colors[ImGuiCol_ButtonHovered] = control_hovered;
    style.Colors[ImGuiCol_ButtonActive] = control_active;
    style.Colors[ImGuiCol_Header] = ImVec4(0.15f, 0.15f, 0.17f, 0.85f);
    style.Colors[ImGuiCol_HeaderHovered] = control_hovered;
    style.Colors[ImGuiCol_HeaderActive] = control_active;
    style.Colors[ImGuiCol_CheckboxSelectedBg] = control_active;
    style.Colors[ImGuiCol_CheckMark] = accent;
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.48f, 0.48f, 0.51f, 1.00f);
    style.Colors[ImGuiCol_SliderGrabActive] = accent;
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.05f, 0.05f, 0.06f, 1.00f);
    style.Colors[ImGuiCol_ScrollbarGrab] = control_active;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.34f, 0.34f, 0.37f, 1.00f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.44f, 0.44f, 0.47f, 1.00f);
    style.Colors[ImGuiCol_Tab] = control;
    style.Colors[ImGuiCol_TabHovered] = control_hovered;
    style.Colors[ImGuiCol_TabSelected] = control_active;
    style.Colors[ImGuiCol_TabSelectedOverline] = accent;
    style.Colors[ImGuiCol_TabDimmed] = ImVec4(0.07f, 0.07f, 0.08f, 1.00f);
    style.Colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.15f, 0.15f, 0.17f, 1.00f);
    style.Colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.42f, 0.42f, 0.45f, 1.00f);
    style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.38f, 0.38f, 0.41f, 1.00f);
    style.Colors[ImGuiCol_SeparatorActive] = accent;
    style.Colors[ImGuiCol_InputTextCursor] = accent;
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.34f, 0.34f, 0.37f, 0.70f);
    style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    style.Colors[ImGuiCol_NavCursor] = accent;

    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.FrameBorderSize = 1.0f;
    style.TouchExtraPadding = ImVec2(2.0f, 2.0f);

    style.ScaleAllSizes(scale);
    ImGui::GetIO().FontGlobalScale = scale;
}

void ImGuiManager::RefreshGamepad() {
    if (gamepad != nullptr) {
        SDL_GameControllerClose(gamepad);
    }
    gamepad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            // SDL returns the existing handle if the device is already open.
            gamepad = SDL_GameControllerOpen(i);
            if (gamepad != nullptr) {
                break;
            }
        }
    }
}

bool ImGuiManager::IsLeftStickPressed() {
    if (gamepad != nullptr && SDL_GameControllerGetAttached(gamepad) == SDL_FALSE) {
        RefreshGamepad();
    }
    if (gamepad == nullptr) {
        RefreshGamepad();
    }
    return gamepad != nullptr &&
           SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0;
}

void ImGuiManager::ApplyLeftStickAsDpad() {
    // ImGui reserves item navigation for the D-pad; fold the left stick into it. The union is
    // re-sent every frame so a recentred stick releases the key without cancelling a D-pad press.
    if (gamepad != nullptr && SDL_GameControllerGetAttached(gamepad) == SDL_FALSE) {
        SDL_GameControllerClose(gamepad);
        gamepad = nullptr;
        stick_left = {};
        stick_right = {};
        stick_up = {};
        stick_down = {};
    }
    if (gamepad == nullptr) {
        RefreshGamepad();
        if (gamepad == nullptr) {
            return;
        }
    }

    // Well past half deflection, so resting drift does not move the selection.
    constexpr Sint16 dead_zone = 18000;
    const Sint16 x = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTX);
    const Sint16 y = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTY);

    const auto button = [this](SDL_GameControllerButton b) {
        return SDL_GameControllerGetButton(gamepad, b) != 0;
    };

    ImGuiIO& io = ImGui::GetIO();

    // Pulse the stick so ImGui's key repeat does not drive the selection too fast.
    const float dt = io.DeltaTime > 0.0f ? io.DeltaTime : 1.0f / 60.0f;
    constexpr float initial_delay = 0.40f; // grace before auto-repeat starts
    constexpr float repeat_rate = 0.14f;   // ~7 steps/sec while held

    const auto pulse = [&](StickAxis& state, bool deflected) {
        if (!deflected) {
            state.held = false;
            state.timer = 0.0f;
            return false;
        }
        if (!state.held) {
            state.held = true;
            state.timer = initial_delay;
            return true;
        }
        state.timer -= dt;
        if (state.timer <= 0.0f) {
            state.timer = repeat_rate;
            return true;
        }
        return false;
    };

    const bool step_left = pulse(stick_left, x < -dead_zone);
    const bool step_right = pulse(stick_right, x > dead_zone);
    const bool step_up = pulse(stick_up, y < -dead_zone);
    const bool step_down = pulse(stick_down, y > dead_zone);

    // The physical D-pad is passed through held, so it keeps ImGui's native repeat behaviour.
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, button(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || step_left);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight,
                   button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || step_right);
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, button(SDL_CONTROLLER_BUTTON_DPAD_UP) || step_up);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, button(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || step_down);

    // Suppress the left stick's native scrolling role; the right stick keeps it for the details
    // pane.
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, false, 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, false, 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, false, 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, false, 0.0f);
}

void ImGuiManager::FocusMainMenuBar() {
    if (!initialized) {
        return;
    }
    // The menu layer is only reachable through a hold-combo; focus the bar's window directly.
    ImGuiWindow* menu_bar = ImGui::FindWindowByName("##MainMenuBar");
    if (menu_bar == nullptr || !menu_bar->WasActive) {
        return;
    }
    // SetNavWindow and NavMoveRequestSubmit are internal and only valid during ImGui's nav
    // update.
    ImGui::FocusWindow(menu_bar);
}

void ImGuiManager::EnsureNavFocus() {
    if (!initialized) {
        return;
    }
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr) {
        return;
    }
    // Re-seed focus onto the frontmost window when the focused window closes.
    if (context->NavWindow == nullptr && context->WindowsFocusOrder.Size > 0) {
        ImGuiWindow* window = context->WindowsFocusOrder.back();
        if (window != nullptr && window->WasActive) {
            ImGui::FocusWindow(window);
        }
    }
}

bool ImGuiManager::ProcessEvent(const SDL_Event& event) {
    if (!initialized) {
        return false;
    }

    // The OS hides the InputPane while unfocused; ImGui only resubmits changed caret data.
    if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED &&
        SDL_IsTextInputActive()) {
        SDL_StartTextInput();
    }

    // The local hardware path is the WGI gamepad; keyboard/mouse must not become a control
    // scheme.
    switch (event.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        if (!SDL_IsTextInputActive() || !IsOskEditingKey(event.key.keysym.sym)) {
            return true;
        }

        // Drop hardware modifiers so a blocked key-up cannot leave ImGui's modifier state stuck.
        SDL_Event edit_event = event;
        edit_event.key.keysym.mod = KMOD_NONE;
        ImGui_ImplSDL2_ProcessEvent(&edit_event);
        return true;
    }
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION:
        return true;
    case SDL_TEXTINPUT:
    case SDL_TEXTEDITING:
        if (!SDL_IsTextInputActive()) {
            return true;
        }
        break;
    default:
        break;
    }

    ImGui_ImplSDL2_ProcessEvent(&event);

    // Gamepad input is not claimed here; the frontend decides based on whether a window is open.
    switch (event.type) {
    case SDL_TEXTINPUT:
    case SDL_TEXTEDITING:
        return true;
    default:
        return false;
    }
}

void ImGuiManager::BeginFrame(unsigned framebuffer_width, unsigned framebuffer_height) {
    if (!initialized) {
        return;
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    if (framebuffer_width > 0 && framebuffer_height > 0 && io.DisplaySize.x > 0.0f &&
        io.DisplaySize.y > 0.0f) {
        // mesa creates its swap chain in output pixels, whereas SDL reports the logical CoreWindow
        // size.
        io.DisplayFramebufferScale = {
            static_cast<float>(framebuffer_width) / io.DisplaySize.x,
            static_cast<float>(framebuffer_height) / io.DisplaySize.y,
        };
    }
    const auto remote_mouse = InputCommon::CemuhookUDP::GetRemoteMouseState();
    if (remote_mouse_for_frontend && remote_mouse.active && remote_mouse.sequence != 0 &&
        remote_mouse.sequence != remote_mouse_sequence) {
        remote_mouse_sequence = remote_mouse.sequence;
        remote_mouse_active = true;
    }
    if (remote_mouse_for_frontend && remote_mouse_active && remote_mouse.active) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        io.MouseDrawCursor = true;
        const float normalized_x = (static_cast<float>(remote_mouse.x) + 32768.0f) / 65535.0f;
        const float normalized_y = (static_cast<float>(remote_mouse.y) + 32768.0f) / 65535.0f;
        io.AddMousePosEvent(std::clamp(normalized_x, 0.0f, 1.0f) * io.DisplaySize.x,
                            std::clamp(normalized_y, 0.0f, 1.0f) * io.DisplaySize.y);
        for (int button = 0; button < 3; ++button) {
            io.AddMouseButtonEvent(button, (remote_mouse.buttons & (1U << button)) != 0);
        }
    } else if (remote_mouse_active) {
        remote_mouse_active = false;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        io.MouseDrawCursor = false;
        for (int button = 0; button < 3; ++button) {
            io.AddMouseButtonEvent(button, false);
        }
    }

    const auto remote_keyboard = InputCommon::CemuhookUDP::GetRemoteKeyboardState();
    const auto current_keyboard =
        remote_keyboard.active ? remote_keyboard.bitmap : std::array<u32, 11>{};
    const bool shifted = (current_keyboard[304 / 32] & (1U << (304 % 32))) != 0;
    const auto imgui_key = [&io](int code) {
        switch (code) {
        case 8:
            return ImGuiKey_Backspace;
        case 9:
            return ImGuiKey_Tab;
        case 13:
            return ImGuiKey_Enter;
        case 27:
            return ImGuiKey_Escape;
        case 127:
            return ImGuiKey_Delete;
        case 273:
            return io.WantTextInput ? ImGuiKey_UpArrow : ImGuiKey_GamepadDpadUp;
        case 274:
            return io.WantTextInput ? ImGuiKey_DownArrow : ImGuiKey_GamepadDpadDown;
        case 275:
            return io.WantTextInput ? ImGuiKey_RightArrow : ImGuiKey_GamepadDpadRight;
        case 276:
            return io.WantTextInput ? ImGuiKey_LeftArrow : ImGuiKey_GamepadDpadLeft;
        case 278:
            return ImGuiKey_Home;
        case 279:
            return ImGuiKey_End;
        case 280:
            return ImGuiKey_PageUp;
        case 281:
            return ImGuiKey_PageDown;
        case 304:
            return ImGuiKey_LeftShift;
        case 306:
            return ImGuiKey_LeftCtrl;
        case 308:
            return ImGuiKey_LeftAlt;
        default:
            return ImGuiKey_None;
        }
    };
    for (std::size_t word = 0; word < current_keyboard.size(); ++word) {
        const u32 changed = current_keyboard[word] ^ remote_keyboard_bitmap[word];
        for (u32 bit = 0; bit < 32; ++bit) {
            if ((changed & (1U << bit)) == 0) {
                continue;
            }
            const int code = static_cast<int>(word * 32 + bit);
            const bool pressed = (current_keyboard[word] & (1U << bit)) != 0;
            if (const ImGuiKey key = imgui_key(code); key != ImGuiKey_None) {
                io.AddKeyEvent(key, pressed);
            }
            if (pressed && code >= 32 && code <= 126) {
                unsigned int character = static_cast<unsigned int>(code);
                if (shifted && character >= 'a' && character <= 'z') {
                    character -= 'a' - 'A';
                }
                io.AddInputCharacter(character);
            }
        }
    }
    remote_keyboard_bitmap = current_keyboard;
    remote_keyboard_sequence = remote_keyboard.sequence;
    // Must run between the backend's frame and NewFrame(), which consumes the queued key events.
    ApplyLeftStickAsDpad();
    Widgets::LatchPopupInputOwner();
    ImGui::NewFrame();
    HandleConsoleNavigation();
}

void ImGuiManager::HandleConsoleNavigation() {
    ImGuiIO& io = ImGui::GetIO();

    // Start requests the frontend menu only; the main loop ignores it during gameplay.
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, /*repeat=*/false)) {
        menu_focus_request = true;
    }
    nav_cycle_request = 0;
    (void)io;
}

void ImGuiManager::EndFrame() {
    if (!initialized) {
        return;
    }
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace UWP
