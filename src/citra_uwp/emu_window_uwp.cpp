// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <SDL.h>
// gamingdeviceinformation.h requires calling-convention definitions from windows.h.
// clang-format off
#include <windows.h>
#include <gamingdeviceinformation.h>
// clang-format on

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <tuple>
#include <utility>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.UI.Core.h>

#include "citra_uwp/emu_window_uwp.h"
#include "citra_uwp/keyboard_input.h"
#include "citra_uwp/ui/imgui_manager.h"
#include "citra_uwp/uwp_config.h"
#include "citra_uwp/uwp_settings.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/3ds.h"
#include "core/core.h"
#include "input_common/keyboard.h"
#include "input_common/main.h"
#include "input_common/udp/udp.h"

namespace UWP {

struct EmuWindow_UWP::DisplayModeWatcher {
    std::mutex callback_mutex;
    EmuWindow_UWP* owner = nullptr;
    winrt::Windows::UI::Core::CoreWindow core_window{nullptr};
    winrt::Windows::Graphics::Display::Core::HdmiDisplayInformation hdmi_display{nullptr};
    winrt::event_token size_changed_token{};
    winrt::event_token display_modes_changed_token{};
    bool has_size_changed_handler = false;
    bool has_display_modes_changed_handler = false;
    bool is_xbox = false;
    GAMING_DEVICE_DEVICE_ID device_id = GAMING_DEVICE_DEVICE_ID_NONE;
    unsigned max_output_width = 1920;
    unsigned max_output_height = 1080;
    bool output_configuration_logged = false;
};

namespace {

constexpr float VirtualCursorMin = 0.0f;
constexpr float VirtualCursorMax = 1.0f;

std::pair<unsigned, unsigned> GetXboxOutputLimit(GAMING_DEVICE_DEVICE_ID device_id) {
    switch (device_id) {
    case GAMING_DEVICE_DEVICE_ID_XBOX_ONE:
    case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_S:
        return {1920, 1080};
    case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_S:
        return {2560, 1440};
    case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X:
    case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X_DEVKIT:
    case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X:
    case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X_DEVKIT:
    default:
        // Unknown future Microsoft Xbox models retain the highest currently supported target.
        return {3840, 2160};
    }
}

const char* GetXboxModelName(GAMING_DEVICE_DEVICE_ID device_id) {
    switch (device_id) {
    case GAMING_DEVICE_DEVICE_ID_XBOX_ONE:
        return "Xbox One";
    case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_S:
        return "Xbox One S";
    case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X:
        return "Xbox One X";
    case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X_DEVKIT:
        return "Xbox One X devkit";
    case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_S:
        return "Xbox Series S";
    case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X:
        return "Xbox Series X";
    case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X_DEVKIT:
        return "Xbox Series X devkit";
    default:
        return "Xbox (unknown model)";
    }
}

float ProjectVirtualCursor(float normalized_position, unsigned extent) {
    if (extent <= 1) {
        return 0.0f;
    }
    return std::clamp(normalized_position, VirtualCursorMin, VirtualCursorMax) *
           static_cast<float>(extent - 1);
}

/// A context sharing objects and the primary drawable with the main one, for shader compilation.
class SharedContext : public Frontend::GraphicsContext {
public:
    explicit SharedContext(GLContext ctx_) : ctx{ctx_} {}

    ~SharedContext() override {
        DestroyGLContext(ctx);
    }

    void MakeCurrent() override {
        if (!wglMakeCurrent(ctx.dc, ctx.context)) {
            LOG_ERROR(Frontend, "Making shared GL context current failed: {}", ::GetLastError());
        }
    }

    void DoneCurrent() override {
        // Do not detach the primary context; shader cache setup calls DoneCurrent before handing
        // off.
        if (wglGetCurrentContext() == ctx.context) {
            wglMakeCurrent(nullptr, nullptr);
        }
    }

private:
    GLContext ctx;
};

} // namespace

EmuWindow_UWP::EmuWindow_UWP(Core::System& system_, bool is_secondary_)
    : Frontend::EmuWindow{is_secondary_}, system{system_} {

    // Extra temporary contexts for parallel shader compilation corrupt mesa's heap on Xbox.
    strict_context_required = true;
    LOG_INFO(Frontend, "UWP parallel shared-context shader compilation is disabled");

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        LOG_CRITICAL(Frontend, "SDL_InitSubSystem(VIDEO) failed: {}", SDL_GetError());
        return;
    }

    // On Xbox the CoreWindow is always full-screen; the requested size is only a hint.
    constexpr int default_width = Core::kScreenTopWidth * 2;
    constexpr int default_height = (Core::kScreenTopHeight + Core::kScreenBottomHeight) * 2;
    sdl_window = SDL_CreateWindow("Azahar", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                  default_width, default_height, SDL_WINDOW_SHOWN);
    if (sdl_window == nullptr) {
        LOG_CRITICAL(Frontend, "SDL_CreateWindow failed: {}", SDL_GetError());
        return;
    }

    // Series S advertises a 4K HDMI mode but its UWP rendering target is capped at 1440p.
    if (!InitializeDisplayModeWatcher()) {
        return;
    }

    gl = CreateGLContext();
    if (!gl) {
        return;
    }

    MakeCurrent();
    if (!LoadGLFunctions()) {
        DoneCurrent();
        DestroyGLContext(gl);
        return;
    }

    // UpdateSwapInterval selects one pacing source so two serial limiters cannot slow a title.
    UpdateSwapInterval();

    // mesa presents to the CoreWindow itself; the WSI handle is its pseudo-window token.
    window_info.type = Frontend::WindowSystemType::Windows;
    window_info.render_surface = static_cast<void*>(gl.window);
    window_info.render_surface_scale = 1.0f;

    OnResize();

    is_valid = true;
    LOG_INFO(Frontend, "UWP emu window ready");
}

EmuWindow_UWP::~EmuWindow_UWP() {
    ReleaseVirtualTouch();
    if (cursor_gamepad != nullptr) {
        SDL_GameControllerClose(cursor_gamepad);
        cursor_gamepad = nullptr;
    }
    ShutdownDisplayModeWatcher();
    DestroyGLContext(gl);
    if (sdl_window != nullptr) {
        SDL_DestroyWindow(sdl_window);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void EmuWindow_UWP::OnResize() {
    // The HDMI API reports output pixels, whereas SDL's window size follows the CoreWindow bounds
    // and can remain at the logical size after a display-mode change.
    if (!RefreshHostSurfaceSize(false)) {
        return;
    }

    const std::uint64_t cached_size = cached_host_surface_size.load(std::memory_order_acquire);
    unsigned width = static_cast<unsigned>(cached_size >> 32);
    unsigned height = static_cast<unsigned>(cached_size & 0xFFFFFFFFu);
    if ((width == 0 || height == 0) && sdl_window != nullptr) {
        int sdl_width = 0;
        int sdl_height = 0;
        SDL_GetWindowSize(sdl_window, &sdl_width, &sdl_height);
        width = static_cast<unsigned>(std::max(sdl_width, 0));
        height = static_cast<unsigned>(std::max(sdl_height, 0));
    }
    if (width == 0 || height == 0) {
        return;
    }
    UpdateCurrentFramebufferLayout(width, height);
    const auto& layout = GetFramebufferLayout();
    LOG_DEBUG(Frontend, "UWP framebuffer {}x{}: top={} [{},{},{},{}], bottom={} [{},{},{},{}]",
              width, height, layout.top_screen_enabled, layout.top_screen.left,
              layout.top_screen.top, layout.top_screen.right, layout.top_screen.bottom,
              layout.bottom_screen_enabled, layout.bottom_screen.left, layout.bottom_screen.top,
              layout.bottom_screen.right, layout.bottom_screen.bottom);
}

bool EmuWindow_UWP::InitializeDisplayModeWatcher() {
    display_mode_watcher = std::make_shared<DisplayModeWatcher>();
    display_mode_watcher->owner = this;

    GAMING_DEVICE_MODEL_INFORMATION device_info{};
    const HRESULT device_result = GetGamingDeviceModelInformation(&device_info);
    if (SUCCEEDED(device_result) && device_info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT) {
        display_mode_watcher->is_xbox = true;
        display_mode_watcher->device_id = device_info.deviceId;
        std::tie(display_mode_watcher->max_output_width, display_mode_watcher->max_output_height) =
            GetXboxOutputLimit(device_info.deviceId);
    } else if (FAILED(device_result)) {
        LOG_WARNING(Frontend, "Xbox model query failed: 0x{:08X}",
                    static_cast<unsigned>(device_result));
    }

    try {
        display_mode_watcher->core_window =
            winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread();
        if (display_mode_watcher->core_window) {
            const auto watcher = display_mode_watcher;
            display_mode_watcher->size_changed_token =
                display_mode_watcher->core_window.SizeChanged([watcher](const auto&, const auto&) {
                    std::scoped_lock lock{watcher->callback_mutex};
                    if (watcher->owner != nullptr) {
                        watcher->owner->RefreshHostSurfaceSize(true);
                    }
                });
            display_mode_watcher->has_size_changed_handler = true;
        }
    } catch (const winrt::hresult_error& error) {
        LOG_WARNING(Frontend, "Could not subscribe to CoreWindow size changes: {}",
                    winrt::to_string(error.message()));
    }

    try {
        display_mode_watcher->hdmi_display =
            winrt::Windows::Graphics::Display::Core::HdmiDisplayInformation::GetForCurrentView();
        if (display_mode_watcher->hdmi_display) {
            const auto watcher = display_mode_watcher;
            display_mode_watcher->display_modes_changed_token =
                display_mode_watcher->hdmi_display.DisplayModesChanged(
                    [watcher](const auto&, const auto&) {
                        std::scoped_lock lock{watcher->callback_mutex};
                        if (watcher->owner != nullptr) {
                            watcher->owner->RefreshHostSurfaceSize(true);
                        }
                    });
            display_mode_watcher->has_display_modes_changed_handler = true;
        }
    } catch (const winrt::hresult_error& error) {
        // HdmiDisplayInformation is Xbox-specific; a PC falls back to the CoreWindow size.
        LOG_INFO(Frontend, "Raw HDMI display information is unavailable: {}",
                 winrt::to_string(error.message()));
    }

    return RefreshHostSurfaceSize(false);
}

void EmuWindow_UWP::ShutdownDisplayModeWatcher() {
    if (!display_mode_watcher) {
        return;
    }

    const auto watcher = display_mode_watcher;
    {
        // Wait for any in-flight event; clearing owner makes an already-queued call a no-op.
        std::scoped_lock lock{watcher->callback_mutex};
        watcher->owner = nullptr;
    }

    try {
        if (watcher->has_display_modes_changed_handler && watcher->hdmi_display) {
            watcher->hdmi_display.DisplayModesChanged(watcher->display_modes_changed_token);
        }
        if (watcher->has_size_changed_handler && watcher->core_window) {
            watcher->core_window.SizeChanged(watcher->size_changed_token);
        }
    } catch (const winrt::hresult_error& error) {
        LOG_WARNING(Frontend, "Could not detach a UWP display event handler: {}",
                    winrt::to_string(error.message()));
    }
    display_mode_watcher.reset();
}

bool EmuWindow_UWP::RefreshHostSurfaceSize(bool request_layout) {
    unsigned width = 0;
    unsigned height = 0;
    unsigned display_width = 0;
    unsigned display_height = 0;

    if (display_mode_watcher) {
        if (display_mode_watcher->is_xbox) {
            try {
                if (display_mode_watcher->hdmi_display) {
                    const auto mode = display_mode_watcher->hdmi_display.GetCurrentDisplayMode();
                    if (mode) {
                        display_width = mode.ResolutionWidthInRawPixels();
                        display_height = mode.ResolutionHeightInRawPixels();
                    }
                }
            } catch (const winrt::hresult_error& error) {
                LOG_WARNING(Frontend, "Reading the current HDMI display mode failed: {}",
                            winrt::to_string(error.message()));
            }

            if (display_width > 0 && display_height > 0) {
                width = std::min(display_width, display_mode_watcher->max_output_width);
                height = std::min(display_height, display_mode_watcher->max_output_height);
            } else {
                width = display_mode_watcher->max_output_width;
                height = display_mode_watcher->max_output_height;
            }

            if (!SetMesaUwpOutputSize(width, height)) {
                return false;
            }
        }

        if ((width == 0 || height == 0) && display_mode_watcher->core_window) {
            try {
                const auto bounds = display_mode_watcher->core_window.Bounds();
                width = static_cast<unsigned>(std::max(bounds.Width, 1.0f));
                height = static_cast<unsigned>(std::max(bounds.Height, 1.0f));
            } catch (const winrt::hresult_error& error) {
                LOG_WARNING(Frontend, "Reading CoreWindow bounds failed: {}",
                            winrt::to_string(error.message()));
            }
        }
    }

    if ((width == 0 || height == 0) && sdl_window != nullptr) {
        int sdl_width = 0;
        int sdl_height = 0;
        SDL_GetWindowSize(sdl_window, &sdl_width, &sdl_height);
        width = static_cast<unsigned>(std::max(sdl_width, 0));
        height = static_cast<unsigned>(std::max(sdl_height, 0));
    }
    if (width == 0 || height == 0) {
        return false;
    }

    const std::uint64_t packed_size =
        (static_cast<std::uint64_t>(width) << 32) | static_cast<std::uint64_t>(height);
    const std::uint64_t previous =
        cached_host_surface_size.exchange(packed_size, std::memory_order_acq_rel);
    if (display_mode_watcher && display_mode_watcher->is_xbox &&
        (!display_mode_watcher->output_configuration_logged || previous != packed_size)) {
        display_mode_watcher->output_configuration_logged = true;
        LOG_INFO(Frontend, "UWP output for {}: HDMI {}x{}, target {}x{} (maximum {}x{})",
                 GetXboxModelName(display_mode_watcher->device_id), display_width, display_height,
                 width, height, display_mode_watcher->max_output_width,
                 display_mode_watcher->max_output_height);
    }
    if (request_layout && previous != packed_size) {
        LOG_INFO(Frontend, "UWP host surface changed to {}x{}", width, height);
        // The main loop consumes this only after pausing/synchronizing the emulation worker.
        layout_update_requested.store(true, std::memory_order_release);
    }
    return true;
}

void EmuWindow_UWP::UpdateSwapInterval() {
    // VSync is a suitable limiter only at normal speed.
    constexpr double normal_speed = 100.0;
    const bool request_vsync =
        Settings::values.use_vsync.GetValue() && Settings::GetFrameLimit() == normal_speed;
    const int requested_interval = request_vsync ? 1 : 0;
    if (requested_interval == applied_swap_interval) {
        return;
    }

    // Remember the attempt so an unsupported extension does not warn every frame.
    applied_swap_interval = requested_interval;
    const bool applied = SetGLSwapInterval(requested_interval);
    vsync_paces_emulation.store(applied && requested_interval == 1, std::memory_order_relaxed);
    if (request_vsync && !applied) {
        LOG_WARNING(Frontend, "VSync could not be enabled; retaining the emulator frame limiter");
    }
}

void EmuWindow_UWP::PollEvents() {
    // The frontend pumps SDL itself with ImGui attached; draining events here would bypass it.
}

void EmuWindow_UWP::PollEvents(ImGuiManager* imgui) {
    KeyboardInput::BeginKeyPresses();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Hotkeys see every key press, whichever layer handles the key.
        if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) &&
            event.key.keysym.sym == SDLK_UNKNOWN) {
            continue;
        }
        if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
            KeyboardInput::AddKeyPress(
                {event.key.keysym.sym, KeyboardInput::ModifiersFromSdl(event.key.keysym.mod)});
        }
        if (imgui != nullptr && imgui->ProcessEvent(event)) {
            // Consumed by the UI (e.g. typing into a text field); don't also act on it here.
            continue;
        }

        switch (event.type) {
        case SDL_QUIT:
            LOG_WARNING(Frontend, "SDL_QUIT received");
            should_quit = true;
            break;

        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                event.window.event == SDL_WINDOWEVENT_RESIZED) {
                layout_update_requested.store(true, std::memory_order_release);
            }
            break;

        case SDL_KEYDOWN:
            if (local_input_enabled && event.key.repeat == 0) {
                InputCommon::GetKeyboard()->PressKey(event.key.keysym.sym);
            }
            break;
        case SDL_KEYUP:
            InputCommon::GetKeyboard()->ReleaseKey(event.key.keysym.sym);
            break;

        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            OnMouseEvent(event);
            break;

        case SDL_DROPFILE:
            if (event.drop.file != nullptr) {
                dropped_files.emplace_back(event.drop.file);
                LOG_INFO(Frontend, "Received file activation: {}", dropped_files.back());
                SDL_free(event.drop.file);
            }
            break;

        // Suspension may be the last code to run before termination, so flush the config here.
        case SDL_APP_WILLENTERBACKGROUND:
            LOG_INFO(Frontend, "Entering background");
            entered_background = true;
            UWP::SaveConfig();
            break;
        case SDL_APP_DIDENTERFOREGROUND:
            LOG_INFO(Frontend, "Returning to foreground");
            entered_foreground = true;
            RefreshHostSurfaceSize(true);
            layout_update_requested.store(true, std::memory_order_release);
            break;

        default:
            break;
        }
    }
}

SDL_GameController* EmuWindow_UWP::FindCursorGamepad() {
    if (cursor_gamepad != nullptr && SDL_GameControllerGetAttached(cursor_gamepad)) {
        return cursor_gamepad;
    }
    if (cursor_gamepad != nullptr) {
        SDL_GameControllerClose(cursor_gamepad);
        cursor_gamepad = nullptr;
    }
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
        if (!SDL_IsGameController(index)) {
            continue;
        }
        cursor_gamepad = SDL_GameControllerOpen(index);
        if (cursor_gamepad != nullptr) {
            return cursor_gamepad;
        }
    }
    return nullptr;
}

void EmuWindow_UWP::ReleaseVirtualTouch() {
    if (virtual_cursor_pressed) {
        TouchReleased();
        virtual_cursor_pressed = false;
    }
}

void EmuWindow_UWP::OnMouseEvent(const SDL_Event& event) {
    if (!local_input_enabled) {
        return;
    }
    const bool motion = event.type == SDL_MOUSEMOTION;
    if (motion && event.motion.xrel == 0 && event.motion.yrel == 0) {
        return;
    }
    const int x = motion ? event.motion.x : event.button.x;
    const int y = motion ? event.motion.y : event.button.y;
    mouse_x = static_cast<float>(x);
    mouse_y = static_cast<float>(y);
    mouse_last_used = std::chrono::steady_clock::now();

    // SDL reports the logical CoreWindow size; the layout is in swap chain pixels.
    int window_width = 0;
    int window_height = 0;
    SDL_GetWindowSize(sdl_window, &window_width, &window_height);
    const auto& layout = GetFramebufferLayout();
    if (window_width <= 0 || window_height <= 0) {
        return;
    }
    const unsigned framebuffer_x =
        static_cast<unsigned>(std::max(0, x) * static_cast<s64>(layout.width) / window_width);
    const unsigned framebuffer_y =
        static_cast<unsigned>(std::max(0, y) * static_cast<s64>(layout.height) / window_height);

    // The left button touches the bottom screen, as in the desktop frontend.
    if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
        mouse_touch_pressed = TouchPressed(framebuffer_x, framebuffer_y);
    } else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
        if (mouse_touch_pressed) {
            TouchReleased();
            mouse_touch_pressed = false;
        }
    } else if (motion && mouse_touch_pressed) {
        TouchMoved(framebuffer_x, framebuffer_y);
    }
}

void EmuWindow_UWP::ReleaseLocalInput() {
    InputCommon::GetKeyboard()->ReleaseAllKeys();
    if (mouse_touch_pressed) {
        TouchReleased();
        mouse_touch_pressed = false;
    }
    mouse_last_used = {};
}

bool EmuWindow_UWP::GetMouseCursor(float& x, float& y) const {
    if (!local_input_enabled || mouse_last_used == std::chrono::steady_clock::time_point{}) {
        return false;
    }
    x = mouse_x;
    y = mouse_y;
    return mouse_touch_pressed ||
           std::chrono::steady_clock::now() - mouse_last_used < std::chrono::seconds{3};
}

void EmuWindow_UWP::UpdateVirtualTouchCursor(bool gameplay_input_enabled) {
    // Keys and touches started during gameplay are released before the UI takes over.
    if (local_input_enabled && !gameplay_input_enabled) {
        ReleaseLocalInput();
    }
    local_input_enabled = gameplay_input_enabled;

    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    const auto mode = UWP::values.right_stick_function.GetValue();
    const auto& layout = GetFramebufferLayout();
    if (!gameplay_input_enabled || mode == RightStickFunction::CStick ||
        !layout.bottom_screen_enabled || layout.bottom_screen.GetWidth() == 0 ||
        layout.bottom_screen.GetHeight() == 0) {
        ReleaseVirtualTouch();
        virtual_cursor_visible = false;
        virtual_cursor_last_update = now;
        return;
    }

    if (!virtual_cursor_initialized) {
        // Keep the cursor in logical touchscreen space; the rendered bottom-screen rectangle
        // varies by layout.
        virtual_cursor_x = 0.5f;
        virtual_cursor_y = 0.5f;
        virtual_cursor_initialized = true;
        virtual_cursor_last_moved = now;
    }

    float elapsed = 1.0f / 60.0f;
    if (virtual_cursor_last_update != Clock::time_point{}) {
        elapsed = std::clamp(std::chrono::duration<float>(now - virtual_cursor_last_update).count(),
                             0.0f, 0.1f);
    }
    virtual_cursor_last_update = now;

    constexpr float deadzone = 0.15f;
    const float sensitivity =
        std::clamp(UWP::values.touchscreen_cursor_sensitivity.GetValue(), 0.25f, 4.0f);
    bool moved = false;
    bool pressed = false;

    if (SDL_GameController* gamepad = FindCursorGamepad(); gamepad != nullptr) {
        float stick_x =
            static_cast<float>(SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTX)) /
            32767.0f;
        float stick_y =
            static_cast<float>(SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTY)) /
            32767.0f;
        const float magnitude = std::sqrt(stick_x * stick_x + stick_y * stick_y);
        if (magnitude <= deadzone) {
            stick_x = 0.0f;
            stick_y = 0.0f;
        } else {
            const float scaled = std::min((magnitude - deadzone) / (1.0f - deadzone), 1.0f);
            stick_x = stick_x / magnitude * scaled;
            stick_y = stick_y / magnitude * scaled;
            virtual_cursor_x += stick_x * 3.0f * elapsed * sensitivity;
            virtual_cursor_y += stick_y * 3.0f * elapsed * sensitivity;
            moved = true;
        }
        pressed = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_RIGHTSTICK) != 0;
    }

    // Use DSU relative motion so long touchpad drags are not limited by absolute coordinates.
    const auto remote_mouse = InputCommon::CemuhookUDP::GetRemoteMouseState();
    if (remote_mouse.active && remote_mouse.sequence != 0 &&
        remote_mouse.sequence != remote_mouse_sequence) {
        if (remote_mouse.delta_x != 0 || remote_mouse.delta_y != 0) {
            // Normalize motion to the native touch surface so layouts do not affect sensitivity.
            virtual_cursor_x += static_cast<float>(remote_mouse.delta_x) /
                                static_cast<float>(Core::kScreenBottomWidth) * sensitivity;
            virtual_cursor_y += static_cast<float>(remote_mouse.delta_y) /
                                static_cast<float>(Core::kScreenBottomHeight) * sensitivity;
            moved = true;
        }
        remote_mouse_sequence = remote_mouse.sequence;
    }
    if (remote_mouse.active) {
        pressed = pressed || (remote_mouse.buttons & 1U) != 0;
    }

    virtual_cursor_x = std::clamp(virtual_cursor_x, VirtualCursorMin, VirtualCursorMax);
    virtual_cursor_y = std::clamp(virtual_cursor_y, VirtualCursorMin, VirtualCursorMax);
    if (moved || pressed) {
        virtual_cursor_last_moved = now;
    }
    const bool timed_out = UWP::values.touchscreen_cursor_timeout.GetValue() &&
                           now - virtual_cursor_last_moved >= std::chrono::seconds{4};
    virtual_cursor_visible = !timed_out;

    const float projected_x =
        ProjectVirtualCursor(virtual_cursor_x, layout.bottom_screen.GetWidth());
    const float projected_y =
        ProjectVirtualCursor(virtual_cursor_y, layout.bottom_screen.GetHeight());
    const unsigned framebuffer_x =
        layout.bottom_screen.left + static_cast<unsigned>(std::lround(projected_x));
    const unsigned framebuffer_y =
        layout.bottom_screen.top + static_cast<unsigned>(std::lround(projected_y));
    if (pressed) {
        if (virtual_cursor_pressed) {
            TouchMoved(framebuffer_x, framebuffer_y);
        } else {
            virtual_cursor_pressed = TouchPressed(framebuffer_x, framebuffer_y);
        }
    } else {
        ReleaseVirtualTouch();
    }
}

Frontend::EmuWindow::CursorInfo EmuWindow_UWP::GetCursorInfo() const {
    const auto& layout = GetFramebufferLayout();
    return {virtual_cursor_visible,
            ProjectVirtualCursor(virtual_cursor_x, layout.bottom_screen.GetWidth()),
            ProjectVirtualCursor(virtual_cursor_y, layout.bottom_screen.GetHeight())};
}

std::unique_ptr<Frontend::GraphicsContext> EmuWindow_UWP::CreateSharedContext() const {
    GLContext shared = CreateGLContext(&gl);
    if (!shared) {
        LOG_ERROR(Frontend, "Failed to create shared GL context; "
                            "background shader compilation is unavailable");
        return nullptr;
    }
    return std::make_unique<SharedContext>(shared);
}

void EmuWindow_UWP::SaveContext() {
    saved_dc = wglGetCurrentDC();
    saved_context = wglGetCurrentContext();
}

void EmuWindow_UWP::RestoreContext() {
    if (saved_context != nullptr && wglGetCurrentContext() != saved_context &&
        !wglMakeCurrent(saved_dc, saved_context)) {
        LOG_ERROR(Frontend, "Restoring saved GL context failed: {}", ::GetLastError());
    }
    saved_dc = nullptr;
    saved_context = nullptr;
}

void EmuWindow_UWP::MakeCurrent() {
    if (!wglMakeCurrent(gl.dc, gl.context)) {
        LOG_ERROR(Frontend, "wglMakeCurrent failed: {}", ::GetLastError());
    }
}

void EmuWindow_UWP::DoneCurrent() {
    if (wglGetCurrentContext() == gl.context) {
        wglMakeCurrent(nullptr, nullptr);
    }
}

void EmuWindow_UWP::SwapBuffers() {
    UpdateSwapInterval();
    wglSwapBuffers(gl.dc);
}

std::optional<std::string> EmuWindow_UWP::TakeDroppedFile() {
    if (dropped_files.empty()) {
        return std::nullopt;
    }
    std::string path = std::move(dropped_files.front());
    dropped_files.pop_front();
    return path;
}

bool EmuWindow_UWP::TakeEnteredBackground() {
    const bool value = entered_background;
    entered_background = false;
    return value;
}

bool EmuWindow_UWP::TakeEnteredForeground() {
    const bool value = entered_foreground;
    entered_foreground = false;
    return value;
}

} // namespace UWP
