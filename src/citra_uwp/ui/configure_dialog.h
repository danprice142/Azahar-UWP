// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "citra_uwp/keyboard_input.h"
#include "citra_uwp/live_camera.h"
#include "common/common_types.h"
#include "input_common/main.h"
#include "input_common/udp/client.h"

namespace Core {
class System;
}

namespace Service::CFG {
class Module;
}

namespace UWP {

/**
 * The settings window, mirroring citra_qt's ConfigureDialog. There is no Apply step: widgets
 * write straight through to Settings::values and the configuration is saved on close.
 */
class ConfigureDialog {
public:
    /// Draws the window. The caller must have synchronized the emulation worker.
    void Draw(Core::System& system);

    void Open();

    /// Opens the title-specific editor; keep_overrides_active is true for the running title.
    void OpenPerGame(u64 title_id, std::string application_path,
                     bool keep_overrides_active = false);

    bool IsOpen() const {
        return is_open;
    }

    [[nodiscard]] bool IsPerGame() const {
        return per_game_mode;
    }

    bool TakeDiscordPresenceRequest() {
        const bool requested = show_discord_presence;
        show_discord_presence = false;
        return requested;
    }

private:
    void DrawGeneralTab();
    void DrawSystemTab();
    void DrawSystemProfile();
    void LoadSystemProfile(Core::System& system);
    void ApplySystemProfile();
    void RegenerateConsoleId();
    void RegenerateMacAddress();
    void DrawInputTab();
    void DrawButtonRow(const char* label, int button_index);
    void DrawAnalogRow(const char* label, int analog_index);
    void DrawControllerHotkeysTab();
    void DrawGraphicsTab();
    void DrawEnhancementsTab();
    void DrawLayoutTab();
    void DrawAudioTab();
    void DrawCameraTab();
    void DrawDebugTab();
    void DrawStorageTab();
    void DrawNetworkTab();
    void DrawUITab();
    void FinishClose();

    bool is_open = false;
    bool per_game_mode = false;
    bool keep_per_game_overrides = false;
    bool reset_per_game_pending = false;
    bool per_game_was_reset = false;
    u64 per_game_title_id = 0;
    std::string per_game_application_path;
    /// Tab focused by controller navigation; applied as selection on the following frame.
    std::string pending_tab_selection;
    /// Tab whose content is visible; LB/RB uses this even when focus is inside a page.
    std::string active_tab_selection;
    /// Keeps the focus request paired with a shoulder-selected tab until ImGui shows it.
    bool pending_tab_focus = false;
    /// Whether controller focus was on the tab row last frame.
    bool tab_row_focused = false;

    bool show_discord_presence = false;

    /// Index of the binding waiting to capture an input, or -1 when idle.
    void StartPolling(bool analog);
    void StopPolling();
    [[nodiscard]] bool IsBindingCaptureActive() const;
    void DrawBindingCapturePopup();
    void UpdatePolling();
    enum class KeyCaptureTarget { None, Button, Analog, Hotkey };
    void StartKeyCapture(KeyCaptureTarget target, int index);
    void UpdateKeyCapture();
    /// Applies SDL's standard controller map for the selected controller.
    bool MapController(const Common::ParamPackage& controller);
    /// Rewrites SDL bindings to honour the selected all/GUID/GUID+port mapping scope.
    void ApplyMappingScope();
    void ReloadDsu();

    std::vector<std::unique_ptr<InputCommon::Polling::DevicePoller>> pollers;

    /// Index of the binding waiting to capture an input, or -1 when idle.
    int polling_button = -1;
    int polling_analog = -1;
    int polling_hotkey = -1;
    int polling_menu_chord = -1;
    bool polling_auto_map = false;
    bool binding_capture_popup_pending = false;
    KeyCaptureTarget key_capture = KeyCaptureTarget::None;
    int key_capture_index = -1;
    /// Analog directions are captured in order: up, down, left, right, modifier.
    std::size_t key_capture_step = 0;
    KeyboardInput::AnalogKeys key_capture_analog;

    struct DsuAsyncState {
        // 0 idle, 1 pending, 2 success, 3 failure.
        std::atomic<int> test_result{0};
        // 0 idle, 1 waiting, 2 ready, 3 first point captured, 4 completed.
        std::atomic<int> calibration_status{0};
        std::atomic<int> min_x{100};
        std::atomic<int> min_y{50};
        std::atomic<int> max_x{1800};
        std::atomic<int> max_y{850};
        std::atomic<bool> calibration_ready{false};
    };
    std::shared_ptr<DsuAsyncState> dsu_async = std::make_shared<DsuAsyncState>();
    std::unique_ptr<InputCommon::CemuhookUDP::CalibrationConfigurationJob> calibration_job;

    struct SystemProfileState {
        std::array<char, 64> username{};
        int birth_month = 1;
        int birth_day = 1;
        int language = 1;
        int sound_mode = 1;
        int country = 49;
        int play_coins = 0;
        bool setup_needed = false;
        u64 console_id = 0;
        std::string mac_address;
    };

    /// Loaded lazily by Draw().
    std::shared_ptr<Service::CFG::Module> system_profile_cfg;
    SystemProfileState system_profile;
    SystemProfileState original_system_profile;
    bool system_profile_loaded = false;
    bool system_profile_editable = false;
    std::string system_profile_status;

    /// Cached microphone enumeration. Querying WASAPI every frame is expensive on Xbox.
    int microphone_devices_type = -1;
    std::vector<std::string> microphone_devices;

    /// Camera enumeration uses async WinRT APIs and runs off the CoreWindow thread.
    std::future<std::vector<LiveCameraDevice>> camera_devices_future;
    std::vector<LiveCameraDevice> camera_devices;
    bool camera_devices_scanned = false;
};

} // namespace UWP
