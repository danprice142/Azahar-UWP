// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "common/settings.h"

namespace UWP {

constexpr std::size_t MaxRecentFiles = 10;
constexpr std::size_t MaxFavoriteTitles = 1024;

/// Keep these persisted values identical to UISettings::GameListText in the desktop frontend.
enum class GameListText : s32 {
    NoText = -1,
    FileName,
    FullPath,
    TitleName,
    TitleID,
    LongTitleName,
    ListEnd,
};

enum class ScreenScraperBoxArt : s32 {
    SmdhIcon,
    Box2D,
    Box3D,
    MixRecalboxV1,
    MixRecalboxV2,
    FanArt,
    Screenshot,
    GameLogo,
    Box2DBack,
    Support2D,
};

enum class ScreenScraperBackground : s32 {
    Off,
    FanArt,
    Screenshot,
    Video,
    MixRecalboxV1,
    MixRecalboxV2,
};

/// Selects how the right analog stick controls the C-Stick and touchscreen cursor.
enum class RightStickFunction : s32 {
    CStickAndTouchscreen,
    CStick,
    Touchscreen,
};

/// Frontend commands a controller hotkey can be bound to.
enum class ControllerHotkey : std::size_t {
    TogglePause,
    StopEmulation,
    QuickSave,
    QuickLoad,
    CaptureScreenshot,
    SwapScreens,
    RestartEmulation,
    AdvanceFrame,
    ToggleUprightScreens,
    CycleLayout,
    ToggleMute,
    IncreaseSpeed,
    DecreaseSpeed,
    ResetSpeed,
    Increase3D,
    Decrease3D,
    Count,
};

constexpr const char* ControllerHotkeyName(ControllerHotkey hotkey) {
    switch (hotkey) {
    case ControllerHotkey::TogglePause:
        return "Continue/Pause Emulation";
    case ControllerHotkey::StopEmulation:
        return "Stop Emulation";
    case ControllerHotkey::QuickSave:
        return "Quick Save";
    case ControllerHotkey::QuickLoad:
        return "Quick Load";
    case ControllerHotkey::CaptureScreenshot:
        return "Capture Screenshot";
    case ControllerHotkey::SwapScreens:
        return "Swap Screens";
    case ControllerHotkey::RestartEmulation:
        return "Restart Emulation";
    case ControllerHotkey::AdvanceFrame:
        return "Advance Frame";
    case ControllerHotkey::ToggleUprightScreens:
        return "Toggle Upright Screens";
    case ControllerHotkey::CycleLayout:
        return "Cycle Screen Layout";
    case ControllerHotkey::ToggleMute:
        return "Toggle Audio Mute";
    case ControllerHotkey::IncreaseSpeed:
        return "Increase Speed Limit";
    case ControllerHotkey::DecreaseSpeed:
        return "Decrease Speed Limit";
    case ControllerHotkey::ResetSpeed:
        return "Reset Speed Limit";
    case ControllerHotkey::Increase3D:
        return "Increase 3D Depth";
    case ControllerHotkey::Decrease3D:
        return "Decrease 3D Depth";
    default:
        return "";
    }
}

/// Frontend settings for the UWP interface (citra_qt's uisettings.h depends on Qt types).
struct Values {
    Values();

    // Behaviour
    Settings::Setting<bool> confirm_before_closing{true, "confirmClose"};
    Settings::Setting<bool> pause_when_in_background{false, "pauseWhenInBackground"};
    Settings::Setting<bool> mute_when_in_background{false, "muteWhenInBackground"};

    // Broadcasting
    Settings::Setting<bool> allow_broadcasting{true, "allowBroadcasting"};
    Settings::Setting<bool> auto_start_broadcasts{false, "autoStartBroadcasts"};
    Settings::Setting<bool> allow_third_party_broadcast_control{
        true, "allowThirdPartyAppsToControlBroadcasts"};

    /// Where Capture Screenshot writes to. Empty uses Screenshots under the active data root.
    Settings::SwitchableSetting<std::string> screenshot_path{"", "screenshotPath"};

    // Game list
    Settings::Setting<bool> game_list_hide_no_icon{false, "hideNoIcon"};
    Settings::Setting<bool> game_list_single_line_mode{false, "singleLineMode"};
    Settings::Setting<int> game_list_icon_size{2, "iconSize"};
    Settings::Setting<GameListText> game_list_row_1{GameListText::TitleName, "row1"};
    Settings::Setting<GameListText> game_list_row_2{GameListText::FileName, "row2"};
    Settings::Setting<bool> game_list_favorite_first{true, "favoriteFirst"};
    Settings::Setting<bool> game_list_favorites_only{false, "favoritesOnly"};
    // Touchscreen cursor
    Settings::Setting<RightStickFunction> right_stick_function{
        RightStickFunction::CStickAndTouchscreen, "rightStickFunction"};
    Settings::Setting<float> touchscreen_cursor_sensitivity{1.0f, "touchscreenCursorSensitivity"};
    Settings::Setting<bool> touchscreen_cursor_timeout{true, "touchscreenCursorTimeout"};

    // ScreenScraper
    Settings::Setting<bool> screenscraper_enabled{false, "screenScraperEnabled"};
    Settings::Setting<std::string> screenscraper_username{"", "screenScraperUsername"};
    Settings::Setting<bool> screenscraper_auto_scrape{true, "screenScraperAutoScrape"};
    Settings::Setting<bool> screenscraper_download_metadata{true, "screenScraperDownloadMetadata"};
    Settings::Setting<bool> screenscraper_download_box_2d{true, "screenScraperDownloadBox2D"};
    Settings::Setting<bool> screenscraper_download_box_3d{false, "screenScraperDownloadBox3D"};
    Settings::Setting<bool> screenscraper_download_box_2d_back{false,
                                                               "screenScraperDownloadBox2DBack"};
    Settings::Setting<bool> screenscraper_download_game_logo{false,
                                                             "screenScraperDownloadGameLogo"};
    Settings::Setting<bool> screenscraper_download_support_2d{false,
                                                              "screenScraperDownloadSupport2D"};
    Settings::Setting<bool> screenscraper_download_mix_v1{false, "screenScraperDownloadMixV1"};
    Settings::Setting<bool> screenscraper_download_mix_v2{false, "screenScraperDownloadMixV2"};
    Settings::Setting<bool> screenscraper_download_fanart{true, "screenScraperDownloadFanArt"};
    Settings::Setting<bool> screenscraper_download_screenshot{false,
                                                              "screenScraperDownloadScreenshot"};
    Settings::Setting<bool> screenscraper_download_video{false, "screenScraperDownloadVideo"};
    Settings::Setting<ScreenScraperBoxArt> screenscraper_box_art{ScreenScraperBoxArt::Box2D,
                                                                 "screenScraperBoxArt"};
    Settings::Setting<ScreenScraperBoxArt> screenscraper_large_box_art{ScreenScraperBoxArt::Box2D,
                                                                       "screenScraperLargeBoxArt"};
    Settings::Setting<ScreenScraperBackground> screenscraper_background{
        ScreenScraperBackground::FanArt, "screenScraperBackground"};
    Settings::Setting<std::string> screenscraper_storage{
        "D:\\DevelopmentFiles\\Azahar\\Screenscraper", "screenScraperStorage"};

    /// Directory scanned for titles. Empty means "ask on first run".
    Settings::Setting<std::string> game_directory{"", "gameDirectory"};
    Settings::Setting<bool> game_directory_deep_scan{false, "gameDirectoryDeepScan"};

    // Interface
    Settings::Setting<bool> show_advanced_frametime_info{false, "showAdvancedFrametimeInfo"};

    /// Drawn as a corner FPS/speed overlay.
    Settings::Setting<bool> show_status_overlay{false, "showStatusOverlay"};

    Settings::Setting<bool> show_filter_bar{true, "showFilterBar"};

    /// Shown in-app, since the payload cannot reach Discord from an app container.
    Settings::Setting<bool> enable_discord_presence{false, "enableDiscordPresence"};

    /// Multiplier applied to every ImGui metric.
    Settings::Setting<float> ui_scale{1.0f, "uiScale"};

    // Recent applications
    std::vector<std::string> recent_files;

    // Library state
    /// Program IDs marked as favorites.
    std::vector<u64> favorited_ids;

    /// A validated NCSD/CCI path exposed to the emulated system as the inserted game card.
    Settings::Setting<std::string> inserted_cartridge{"", "insertedCartridge"};

    // Controller hotkeys
    /// Serialized button params per ControllerHotkey slot; empty means unbound.
    std::array<std::string, static_cast<std::size_t>(ControllerHotkey::Count)> controller_hotkeys{};
    /// Two button bindings that must both be held to open the gameplay menu (default View + Menu).
    std::array<std::string, 2> in_game_menu_chord{};
};

extern Values values;

void AddRecentFile(std::string path);

} // namespace UWP
