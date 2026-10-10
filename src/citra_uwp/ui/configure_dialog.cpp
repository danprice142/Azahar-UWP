// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include <SDL.h>
#include <fmt/format.h>
#include <imgui.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>

#include "audio_core/input_details.h"
#include "audio_core/sink.h"
#include "citra_uwp/screenscraper.h"
#include "citra_uwp/storage_paths.h"
#include "citra_uwp/ui/configure_dialog.h"
#include "citra_uwp/ui/virtual_keyboard.h"
#include "citra_uwp/ui/widgets.h"
#include "citra_uwp/uwp_config.h"
#include "citra_uwp/uwp_settings.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/hle/service/ptm/ptm.h"
#include "core/hle/service/service.h"
#include "input_common/udp/udp.h"

namespace UWP {

namespace {

using namespace Widgets;

enum class CustomStorageTarget {
    Applications,
    Nand,
    Sdmc,
    TextureLoad,
    Mods,
    TextureDump,
    Cheats,
    ScreenScraper,
};

struct CustomStorageResult {
    CustomStorageTarget target{};
    std::string path;
    std::string error;
};

struct CustomStoragePickerState {
    std::mutex mutex;
    bool active = false;
    std::optional<CustomStorageResult> result;
};

CustomStoragePickerState custom_storage_picker;

std::wstring_view CustomStorageToken(CustomStorageTarget target) {
    switch (target) {
    case CustomStorageTarget::Applications:
        return L"AzaharCustomApplications";
    case CustomStorageTarget::Nand:
        return L"AzaharCustomNand";
    case CustomStorageTarget::Sdmc:
        return L"AzaharCustomSdmc";
    case CustomStorageTarget::TextureLoad:
        return L"AzaharCustomTextureLoad";
    case CustomStorageTarget::Mods:
        return L"AzaharCustomMods";
    case CustomStorageTarget::TextureDump:
        return L"AzaharCustomTextureDump";
    case CustomStorageTarget::Cheats:
        return L"AzaharCustomCheats";
    case CustomStorageTarget::ScreenScraper:
        return L"AzaharCustomScreenScraper";
    }
    return L"AzaharCustomStorage";
}

winrt::fire_and_forget PickCustomStorageFolder(CustomStorageTarget target) {
    CustomStorageResult result{target};
    try {
        winrt::Windows::Storage::Pickers::FolderPicker picker;
        picker.SettingsIdentifier(winrt::hstring{CustomStorageToken(target)});
        picker.SuggestedStartLocation(
            winrt::Windows::Storage::Pickers::PickerLocationId::ComputerFolder);
        picker.ViewMode(winrt::Windows::Storage::Pickers::PickerViewMode::List);
        picker.FileTypeFilter().Append(L"*");
        const auto folder = co_await picker.PickSingleFolderAsync();
        if (folder) {
            result.path = Common::UTF16ToUTF8(std::wstring{folder.Path().c_str()});
            if (result.path.empty()) {
                result.error = "The selected folder does not expose a usable file-system path.";
            } else {
                winrt::Windows::Storage::AccessCache::StorageApplicationPermissions::
                    FutureAccessList()
                        .AddOrReplace(winrt::hstring{CustomStorageToken(target)}, folder);
            }
        }
    } catch (const winrt::hresult_error& error) {
        result.error = "The folder picker failed: " + winrt::to_string(error.message());
    } catch (const std::exception& error) {
        result.error = "The folder picker failed: " + std::string{error.what()};
    } catch (...) {
        result.error = "The folder picker failed unexpectedly.";
    }

    std::scoped_lock lock{custom_storage_picker.mutex};
    custom_storage_picker.result = std::move(result);
    custom_storage_picker.active = false;
}

bool StartCustomStoragePicker(CustomStorageTarget target) {
    {
        std::scoped_lock lock{custom_storage_picker.mutex};
        if (custom_storage_picker.active) {
            return false;
        }
        custom_storage_picker.active = true;
        custom_storage_picker.result.reset();
    }
    PickCustomStorageFolder(target);
    return true;
}

std::optional<CustomStorageResult> TakeCustomStorageResult() {
    std::scoped_lock lock{custom_storage_picker.mutex};
    auto result = std::move(custom_storage_picker.result);
    custom_storage_picker.result.reset();
    return result;
}

void DrawScreenScraperAccount() {
    GroupHeader("ScreenScraper account");
    ImGui::TextWrapped("Sign in to your own Screenscraper account for boxart, metadata etc.");
    if (!ScreenScraper::HasDeveloperCredentials()) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                           "This package was built without developer credentials.");
    }
    static std::array<char, 128> username{};
    static std::array<char, 128> password{};
    static bool initialized = false;
    static std::future<ScreenScraper::Result> login_future;
    static std::string login_status;
    if (!initialized) {
        const std::string current = UWP::values.screenscraper_username.GetValue();
        std::copy_n(current.begin(), std::min(current.size(), username.size() - 1),
                    username.begin());
        initialized = true;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    GetVirtualKeyboard().InputText("Username##screenscraper", username.data(), username.size(),
                                   "ScreenScraper username");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    GetVirtualKeyboard().InputText("Password##screenscraper", password.data(), password.size(),
                                   "ScreenScraper password", true);
    const bool testing = login_future.valid();
    ImGui::BeginDisabled(testing || !ScreenScraper::HasDeveloperCredentials());
    if (ImGui::Button(ScreenScraper::HasUserCredentials() ? "Update sign-in" : "Sign in")) {
        std::string error;
        if (ScreenScraper::SaveUserCredentials(username.data(), password.data(), error)) {
            std::fill(password.begin(), password.end(), '\0');
            login_status = "Checking credentials...";
            login_future = std::async(std::launch::async, ScreenScraper::TestLogin);
        } else {
            login_status = std::move(error);
        }
    }
    ImGui::EndDisabled();
    if (ScreenScraper::HasUserCredentials()) {
        ImGui::SameLine();
        if (ImGui::Button("Sign out")) {
            ScreenScraper::ClearUserCredentials();
            UWP::values.screenscraper_username.SetValue("");
            std::fill(username.begin(), username.end(), '\0');
            login_status = "Signed out.";
        }
    }
    if (login_future.valid() &&
        login_future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        try {
            const ScreenScraper::Result result = login_future.get();
            login_status = result.message;
            if (result.success && result.authenticated) {
                UWP::values.screenscraper_enabled.SetValue(true);
                UWP::SaveConfig();
                LOG_INFO(Frontend, "ScreenScraper account authenticated; library data enabled");
            } else {
                // A stored credential is not proof that ScreenScraper accepted it.
                ScreenScraper::ClearUserCredentials();
                UWP::values.screenscraper_username.SetValue("");
                UWP::values.screenscraper_enabled.SetValue(false);
                UWP::SaveConfig();
                LOG_WARNING(Frontend, "ScreenScraper account authentication failed: {}",
                            login_status);
            }
        } catch (const std::exception& exception) {
            login_status = std::string{"ScreenScraper sign-in failed: "} + exception.what();
            ScreenScraper::ClearUserCredentials();
            UWP::values.screenscraper_username.SetValue("");
            UWP::values.screenscraper_enabled.SetValue(false);
            UWP::SaveConfig();
            LOG_WARNING(Frontend, "{}", login_status);
        } catch (...) {
            login_status = "ScreenScraper sign-in failed unexpectedly.";
            ScreenScraper::ClearUserCredentials();
            UWP::values.screenscraper_username.SetValue("");
            UWP::values.screenscraper_enabled.SetValue(false);
            UWP::SaveConfig();
            LOG_WARNING(Frontend, "{}", login_status);
        }
    }
    if (!login_status.empty()) {
        ImGui::TextWrapped("%s", login_status.c_str());
    }
}

// Combo contents follow the item order of the corresponding .ui files (indexed by enum value).
constexpr std::array kTextureFilterItems{"None",       "Anime4K", "Bicubic",
                                         "ScaleForce", "xBRZ",    "MMPX"};
constexpr std::array kTextureSamplingItems{"Game Controlled", "Nearest Neighbor", "Linear"};
constexpr std::array kResolutionItems{
    "Auto (Window Size)",     "Native (400x240)",       "2x Native (800x480)",
    "3x Native (1200x720)",   "4x Native (1600x960)",   "5x Native (2000x1200)",
    "6x Native (2400x1440)",  "7x Native (2800x1680)",  "8x Native (3200x1920)",
    "9x Native (3600x2160)",  "10x Native (4000x2400)", "11x Native (4400x2640)",
    "12x Native (4800x2880)", "13x Native (5200x3120)", "14x Native (5600x3360)",
    "15x Native (6000x3600)", "16x Native (6400x3840)", "17x Native (6800x4080)",
    "18x Native (7200x4320)",
};
constexpr std::array kStereoItems{"Off",         "Side by Side", "Side by Side (Full)",
                                  "Anaglyph",    "Interlaced",   "Reverse Interlaced",
                                  "Cardboard VR"};
constexpr std::array kMonoItems{"Left Eye (default)", "Right Eye"};
// SeparateWindows is not exposed; a UWP app has one CoreWindow.
constexpr std::array kLayoutItems{"Default",      "Single Screen", "Large Screen",
                                  "Side by Side", "Hybrid Screen", "Custom Layout"};
constexpr std::array kLayoutValues{
    Settings::LayoutOption::Default,      Settings::LayoutOption::SingleScreen,
    Settings::LayoutOption::LargeScreen,  Settings::LayoutOption::SideScreen,
    Settings::LayoutOption::HybridScreen, Settings::LayoutOption::CustomLayout};
constexpr std::array kSmallScreenPosItems{"Top Right",   "Middle Right", "Bottom Right",
                                          "Top Left",    "Middle Left",  "Bottom Left",
                                          "Above Large", "Below Large"};
constexpr std::array kAudioEmulationItems{"HLE", "LLE", "LLE (Multithreaded)"};
constexpr std::array kInitClockItems{"System Time", "Fixed Time"};
constexpr std::array kInitTicksItems{"Random", "Fixed"};
constexpr std::array kMonthItems{"January",   "February", "March",    "April",
                                 "May",       "June",     "July",     "August",
                                 "September", "October",  "November", "December"};
constexpr std::array kDaysInMonth{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
constexpr std::array kLanguageItems{
    "Japanese",           "English", "French", "German",     "Italian", "Spanish",
    "Simplified Chinese", "Korean",  "Dutch",  "Portuguese", "Russian", "Traditional Chinese",
};
constexpr std::array kSoundModeItems{"Mono", "Stereo", "Surround"};
constexpr std::array kCameraFlipItems{"None", "Horizontal", "Vertical", "Reverse"};

/// Region list as ordered in configure_system.ui; -1 (auto-select) is handled by the caller.
constexpr std::array kRegionItems{"Japan", "USA",   "Europe", "Australia",
                                  "China", "Korea", "Taiwan"};

struct CountryOption {
    u8 code;
    const char* name;
};

constexpr std::array kCountryItems{
    CountryOption{1, "Japan"},
    CountryOption{8, "Anguilla"},
    CountryOption{9, "Antigua and Barbuda"},
    CountryOption{10, "Argentina"},
    CountryOption{11, "Aruba"},
    CountryOption{12, "Bahamas"},
    CountryOption{13, "Barbados"},
    CountryOption{14, "Belize"},
    CountryOption{15, "Bolivia"},
    CountryOption{16, "Brazil"},
    CountryOption{17, "British Virgin Islands"},
    CountryOption{18, "Canada"},
    CountryOption{19, "Cayman Islands"},
    CountryOption{20, "Chile"},
    CountryOption{21, "Colombia"},
    CountryOption{22, "Costa Rica"},
    CountryOption{23, "Dominica"},
    CountryOption{24, "Dominican Republic"},
    CountryOption{25, "Ecuador"},
    CountryOption{26, "El Salvador"},
    CountryOption{27, "French Guiana"},
    CountryOption{28, "Grenada"},
    CountryOption{29, "Guadeloupe"},
    CountryOption{30, "Guatemala"},
    CountryOption{31, "Guyana"},
    CountryOption{32, "Haiti"},
    CountryOption{33, "Honduras"},
    CountryOption{34, "Jamaica"},
    CountryOption{35, "Martinique"},
    CountryOption{36, "Mexico"},
    CountryOption{37, "Montserrat"},
    CountryOption{38, "Netherlands Antilles"},
    CountryOption{39, "Nicaragua"},
    CountryOption{40, "Panama"},
    CountryOption{41, "Paraguay"},
    CountryOption{42, "Peru"},
    CountryOption{43, "Saint Kitts and Nevis"},
    CountryOption{44, "Saint Lucia"},
    CountryOption{45, "Saint Vincent and the Grenadines"},
    CountryOption{46, "Suriname"},
    CountryOption{47, "Trinidad and Tobago"},
    CountryOption{48, "Turks and Caicos Islands"},
    CountryOption{49, "United States"},
    CountryOption{50, "Uruguay"},
    CountryOption{51, "US Virgin Islands"},
    CountryOption{52, "Venezuela"},
    CountryOption{64, "Albania"},
    CountryOption{65, "Australia"},
    CountryOption{66, "Austria"},
    CountryOption{67, "Belgium"},
    CountryOption{68, "Bosnia and Herzegovina"},
    CountryOption{69, "Botswana"},
    CountryOption{70, "Bulgaria"},
    CountryOption{71, "Croatia"},
    CountryOption{72, "Cyprus"},
    CountryOption{73, "Czech Republic"},
    CountryOption{74, "Denmark"},
    CountryOption{75, "Estonia"},
    CountryOption{76, "Finland"},
    CountryOption{77, "France"},
    CountryOption{78, "Germany"},
    CountryOption{79, "Greece"},
    CountryOption{80, "Hungary"},
    CountryOption{81, "Iceland"},
    CountryOption{82, "Ireland"},
    CountryOption{83, "Italy"},
    CountryOption{84, "Latvia"},
    CountryOption{85, "Lesotho"},
    CountryOption{86, "Liechtenstein"},
    CountryOption{87, "Lithuania"},
    CountryOption{88, "Luxembourg"},
    CountryOption{89, "Macedonia"},
    CountryOption{90, "Malta"},
    CountryOption{91, "Montenegro"},
    CountryOption{92, "Mozambique"},
    CountryOption{93, "Namibia"},
    CountryOption{94, "Netherlands"},
    CountryOption{95, "New Zealand"},
    CountryOption{96, "Norway"},
    CountryOption{97, "Poland"},
    CountryOption{98, "Portugal"},
    CountryOption{99, "Romania"},
    CountryOption{100, "Russia"},
    CountryOption{101, "Serbia"},
    CountryOption{102, "Slovakia"},
    CountryOption{103, "Slovenia"},
    CountryOption{104, "South Africa"},
    CountryOption{105, "Spain"},
    CountryOption{106, "Swaziland"},
    CountryOption{107, "Sweden"},
    CountryOption{108, "Switzerland"},
    CountryOption{109, "Turkey"},
    CountryOption{110, "United Kingdom"},
    CountryOption{111, "Zambia"},
    CountryOption{112, "Zimbabwe"},
    CountryOption{113, "Azerbaijan"},
    CountryOption{114, "Mauritania"},
    CountryOption{115, "Mali"},
    CountryOption{116, "Niger"},
    CountryOption{117, "Chad"},
    CountryOption{118, "Sudan"},
    CountryOption{119, "Eritrea"},
    CountryOption{120, "Djibouti"},
    CountryOption{121, "Somalia"},
    CountryOption{122, "Andorra"},
    CountryOption{123, "Gibraltar"},
    CountryOption{124, "Guernsey"},
    CountryOption{125, "Isle of Man"},
    CountryOption{126, "Jersey"},
    CountryOption{127, "Monaco"},
    CountryOption{128, "Taiwan"},
    CountryOption{136, "South Korea"},
    CountryOption{144, "Hong Kong"},
    CountryOption{145, "Macau"},
    CountryOption{152, "Indonesia"},
    CountryOption{153, "Singapore"},
    CountryOption{154, "Thailand"},
    CountryOption{155, "Philippines"},
    CountryOption{156, "Malaysia"},
    CountryOption{160, "China"},
    CountryOption{168, "United Arab Emirates"},
    CountryOption{169, "India"},
    CountryOption{170, "Egypt"},
    CountryOption{171, "Oman"},
    CountryOption{172, "Qatar"},
    CountryOption{173, "Kuwait"},
    CountryOption{174, "Saudi Arabia"},
    CountryOption{175, "Syria"},
    CountryOption{176, "Bahrain"},
    CountryOption{177, "Jordan"},
    CountryOption{184, "San Marino"},
    CountryOption{185, "Vatican City"},
    CountryOption{186, "Bermuda"},
};

const CountryOption* FindCountry(int code) {
    const auto found =
        std::find_if(kCountryItems.begin(), kCountryItems.end(),
                     [code](const CountryOption& country) { return country.code == code; });
    return found == kCountryItems.end() ? nullptr : &*found;
}

std::u16string NormalizeUsername(std::string_view username) {
    std::u16string converted = Common::UTF8ToUTF16(username);
    if (converted.size() <= 10) {
        return converted;
    }

    std::size_t length = 10;
    // Do not leave half of a UTF-16 surrogate pair at the end of the 3DS' ten-code-unit field.
    const char16_t final = converted[length - 1];
    if (final >= 0xD800 && final <= 0xDBFF) {
        --length;
    }
    converted.resize(length);
    return converted;
}

void StoreUsername(std::array<char, 64>& destination, std::u16string_view username) {
    const std::string utf8 = Common::UTF16ToUTF8(username);
    destination.fill('\0');
    std::copy_n(utf8.begin(), std::min(utf8.size(), destination.size() - 1), destination.begin());
}

template <typename Type, bool ranged, typename Draw>
bool DrawSwitchableSetting(bool per_game_mode, const char* id,
                           Settings::SwitchableSetting<Type, ranged>& setting, Draw&& draw) {
    if (!per_game_mode) {
        // As on desktop, an option supplied by the running title is visible but locked.
        ImGui::BeginDisabled(!setting.UsingGlobal());
        const bool changed = draw();
        ImGui::EndDisabled();
        return changed;
    }

    ImGui::PushID(id);
    bool override_global = !setting.UsingGlobal();
    bool changed = ImGui::Checkbox("Override global", &override_global);
    if (changed) {
        if (override_global) {
            const Type inherited_value = setting.GetValue(/*need_global=*/true);
            setting.SetGlobal(false);
            setting.SetValue(inherited_value);
        } else {
            setting.SetGlobal(true);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!override_global);
    changed = draw() || changed;
    ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}

} // namespace

void ConfigureDialog::Open() {
    Settings::SetConfiguringGlobal(true);
    per_game_mode = false;
    keep_per_game_overrides = false;
    reset_per_game_pending = false;
    per_game_was_reset = false;
    per_game_title_id = 0;
    per_game_application_path.clear();
    pending_tab_selection = "General";
    active_tab_selection = "General";
    pending_tab_focus = false;
    tab_row_focused = false;
    system_profile_cfg.reset();
    system_profile_loaded = false;
    system_profile_status.clear();
    is_open = true;
}

void ConfigureDialog::OpenPerGame(u64 title_id, std::string application_path,
                                  bool keep_overrides_active) {
    per_game_title_id = title_id;
    per_game_application_path = std::move(application_path);
    keep_per_game_overrides = keep_overrides_active;
    reset_per_game_pending = false;
    per_game_was_reset = false;
    per_game_mode = true;
    pending_tab_selection = "General";
    active_tab_selection = "General";
    pending_tab_focus = false;
    tab_row_focused = false;
    system_profile_cfg.reset();
    system_profile_loaded = false;
    system_profile_status.clear();
    Settings::SetConfiguringGlobal(false);
    LoadPerGameConfig(per_game_title_id, per_game_application_path);
    is_open = true;
}

void ConfigureDialog::FinishClose() {
    StopPolling();
    polling_button = -1;
    polling_analog = -1;
    polling_hotkey = -1;
    polling_menu_chord = -1;
    polling_auto_map = false;
    binding_capture_popup_pending = false;
    calibration_job.reset();
    if (per_game_mode) {
        if (!per_game_was_reset) {
            SavePerGameConfig(per_game_title_id, per_game_application_path);
        }
        if (!keep_per_game_overrides) {
            RestoreGlobalConfig();
        }
    } else {
        system_profile_status.clear();
        ApplySystemProfile();
        if (!system_profile_status.empty()) {
            // Keep the editor visible so a write error remains actionable.
            is_open = true;
            return;
        }
    }
    system_profile_cfg.reset();
    system_profile_loaded = false;
    Settings::SetConfiguringGlobal(true);
}

void ConfigureDialog::Draw(Core::System& system) {
    if (!is_open) {
        return;
    }
    if (!per_game_mode && !system_profile_loaded) {
        LoadSystemProfile(system);
    }
    const bool was_open = is_open;

    SetNextWindowFullArea();
    constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                              ImGuiWindowFlags_NoCollapse |
                                              ImGuiWindowFlags_NoSavedSettings;

    const std::string title =
        per_game_mode
            ? fmt::format("Application Properties - {:016X}###Configure", per_game_title_id)
            : "Configure";
    if (ImGui::Begin(title.c_str(), nullptr, window_flags)) {
        // B backs out, since the title-bar close button is unreachable from a pad.
        HandleGamepadBack(is_open);
        // Binding capture is polled here so Controller Hotkeys work while the Input tab is
        // visible.
        UpdatePolling();
        UpdateKeyCapture();

        if (per_game_mode) {
            ImGui::TextDisabled("Only settings which support per-application overrides are shown.");
            if (!per_game_application_path.empty()) {
                ImGui::TextWrapped("%s", per_game_application_path.c_str());
            }
            if (ImGui::Button("Reset per-application overrides...")) {
                reset_per_game_pending = true;
                ImGui::OpenPopup("Reset per-application settings?");
            }
            if (ImGui::BeginPopupModal("Reset per-application settings?", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoSavedSettings)) {
                ImGui::TextWrapped(
                    "Delete this title's custom settings and use the global configuration?");
                if (ImGui::Button("Reset", ImVec2(160.0f, 0.0f))) {
                    if (DeletePerGameConfig(per_game_title_id, per_game_application_path)) {
                        RestoreGlobalConfig();
                        per_game_was_reset = true;
                        is_open = false;
                    }
                    reset_per_game_pending = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(160.0f, 0.0f)) ||
                    ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
                    reset_per_game_pending = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::Separator();
        }

        std::vector<const char*> tab_labels{"General", "System"};
        if (!per_game_mode) {
            tab_labels.insert(tab_labels.end(), {"Input", "Controller Hotkeys"});
        }
        tab_labels.insert(tab_labels.end(),
                          {"Graphics", "Enhancements", "Layout", "Audio", "Debug"});
        if (!per_game_mode) {
            tab_labels.insert(tab_labels.end(), {"Camera", "Storage", "Network", "UI"});
        }

        const bool binding_capture_active = polling_button >= 0 || polling_analog >= 0 ||
                                            polling_hotkey >= 0 || polling_menu_chord >= 0 ||
                                            polling_auto_map;
        const bool popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        const bool previous_tab =
            ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ||
            (tab_row_focused && ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false));
        const bool next_tab =
            ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ||
            (tab_row_focused && ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false));
        if (!binding_capture_active && !popup_open && previous_tab != next_tab) {
            auto current = std::find(tab_labels.begin(), tab_labels.end(), active_tab_selection);
            std::size_t current_index =
                current == tab_labels.end()
                    ? 0
                    : static_cast<std::size_t>(current - tab_labels.begin());
            if (previous_tab) {
                current_index = (current_index + tab_labels.size() - 1) % tab_labels.size();
            } else {
                current_index = (current_index + 1) % tab_labels.size();
            }
            pending_tab_selection = tab_labels[current_index];
            active_tab_selection = pending_tab_selection;
            pending_tab_focus = true;
        }

        if (ImGui::BeginTabBar("##configure_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
            bool restore_active_tab_focus = false;
            bool tab_focused_this_frame = false;
            const auto begin_tab = [this, &restore_active_tab_focus,
                                    &tab_focused_this_frame](const char* label) {
                const bool select_now = pending_tab_selection == label;
                const bool restore_focus =
                    restore_active_tab_focus && active_tab_selection == label;
                const bool shoulder_focus = pending_tab_focus && select_now;
                if (restore_focus || shoulder_focus) {
                    ImGui::SetKeyboardFocusHere();
                }
                const bool visible = ImGui::BeginTabItem(
                    label, nullptr, select_now ? ImGuiTabItemFlags_SetSelected : 0);
                if (restore_focus || shoulder_focus) {
                    ImGui::SetItemDefaultFocus();
                    restore_active_tab_focus = false;
                }
                // SetSelected may take a frame to become visible; keep the request until ImGui
                // confirms it.
                if (select_now && visible) {
                    pending_tab_selection.clear();
                    pending_tab_focus = false;
                }
                if (visible) {
                    active_tab_selection = label;
                }
                if (ImGui::IsItemFocused()) {
                    tab_focused_this_frame = true;
                }
                if (ImGui::IsItemFocused() && !visible) {
                    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false)) {
                        // Moving up from page content can land on General; return focus to the
                        // selected tab instead.
                        restore_active_tab_focus = true;
                    }
                }
                return visible;
            };
            if (begin_tab("General")) {
                DrawGeneralTab();
                ImGui::EndTabItem();
            }
            if (begin_tab("System")) {
                DrawSystemTab();
                ImGui::EndTabItem();
            }
            if (!per_game_mode) {
                if (begin_tab("Input")) {
                    DrawInputTab();
                    ImGui::EndTabItem();
                }
                if (begin_tab("Controller Hotkeys")) {
                    DrawControllerHotkeysTab();
                    ImGui::EndTabItem();
                }
            }
            if (begin_tab("Graphics")) {
                DrawGraphicsTab();
                ImGui::EndTabItem();
            }
            if (begin_tab("Enhancements")) {
                DrawEnhancementsTab();
                ImGui::EndTabItem();
            }
            if (begin_tab("Layout")) {
                DrawLayoutTab();
                ImGui::EndTabItem();
            }
            if (begin_tab("Audio")) {
                DrawAudioTab();
                ImGui::EndTabItem();
            }
            if (begin_tab("Debug")) {
                DrawDebugTab();
                ImGui::EndTabItem();
            }
            if (!per_game_mode) {
                if (begin_tab("Camera")) {
                    DrawCameraTab();
                    ImGui::EndTabItem();
                }
                if (begin_tab("Storage")) {
                    DrawStorageTab();
                    ImGui::EndTabItem();
                }
                if (begin_tab("Network")) {
                    DrawNetworkTab();
                    ImGui::EndTabItem();
                }
                if (begin_tab("UI")) {
                    DrawUITab();
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
            tab_row_focused = tab_focused_this_frame;
        } else {
            tab_row_focused = false;
        }

        DrawBindingCapturePopup();
    }
    ImGui::End();
    if (was_open && !is_open) {
        FinishClose();
    }
}

void ConfigureDialog::DrawGeneralTab() {
    auto& settings = Settings::values;

    if (!per_game_mode) {
        GroupHeader("General");
        SettingCheckbox("Confirm exit while emulation is running",
                        UWP::values.confirm_before_closing);
        SettingCheckbox("Pause emulation when in background", UWP::values.pause_when_in_background);
        SettingCheckbox("Mute audio when in background", UWP::values.mute_when_in_background);
        SettingCheckbox("Enable Gamemode", settings.enable_gamemode);
    }

    GroupHeader("Emulation");
    // frame_limit is a percentage; 0 means unlimited.
    DrawSwitchableSetting(per_game_mode, "frame_limit", settings.frame_limit, [&] {
        bool changed = false;
        int frame_limit = static_cast<int>(settings.frame_limit.GetValue());
        bool limit_enabled = frame_limit != 0;
        if (ImGui::Checkbox("Limit speed to", &limit_enabled)) {
            settings.frame_limit.SetValue(limit_enabled ? 100.0 : 0.0);
            frame_limit = static_cast<int>(settings.frame_limit.GetValue());
            changed = true;
        }
        if (limit_enabled) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
            if (ImGui::SliderInt("##frame_limit", &frame_limit, 1, 400, "%d%%")) {
                settings.frame_limit.SetValue(static_cast<double>(frame_limit));
                changed = true;
            }
        }
        return changed;
    });

    DrawSwitchableSetting(per_game_mode, "turbo_limit", settings.turbo_limit, [&] {
        int turbo_limit = static_cast<int>(settings.turbo_limit.GetValue());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        const bool changed = ImGui::SliderInt("Turbo speed", &turbo_limit, 1, 400, "%d%%");
        if (changed) {
            settings.turbo_limit.SetValue(static_cast<double>(turbo_limit));
        }
        Tooltip("Speed used while the turbo hotkey is held.");
        return changed;
    });

    GroupHeader("Screenshots");
    DrawSwitchableSetting(per_game_mode, "screenshot_path", UWP::values.screenshot_path, [&] {
        return SettingInputText("Screenshot path", UWP::values.screenshot_path);
    });

    if (!per_game_mode) {
        // Update checking is omitted: UWP packages are serviced by their package source.
        GroupHeader("Updates");
        ImGui::TextDisabled("Updates are delivered through the package source on this platform.");
    }
}

void ConfigureDialog::LoadSystemProfile(Core::System& system) {
    // GetModule() may return the running CFG service; the main loop has synchronized the worker.
    system_profile_editable = !system.IsPoweredOn();
    system_profile_cfg = Service::CFG::GetModule(system);

    StoreUsername(system_profile.username, system_profile_cfg->GetUsername());
    std::tie(system_profile.birth_month, system_profile.birth_day) =
        system_profile_cfg->GetBirthday();
    system_profile.birth_month = std::clamp(system_profile.birth_month, 1, 12);
    system_profile.birth_day =
        std::clamp(system_profile.birth_day, 1, kDaysInMonth[system_profile.birth_month - 1]);
    system_profile.language = std::clamp(static_cast<int>(system_profile_cfg->GetSystemLanguage()),
                                         0, static_cast<int>(kLanguageItems.size() - 1));
    system_profile.sound_mode =
        std::clamp(static_cast<int>(system_profile_cfg->GetSoundOutputMode()), 0,
                   static_cast<int>(kSoundModeItems.size() - 1));
    system_profile.country = system_profile_cfg->GetCountryCode();
    system_profile.setup_needed = system_profile_cfg->IsSystemSetupNeeded();
    system_profile.console_id = system_profile_cfg->GetConsoleUniqueId();
    system_profile.mac_address = system_profile_cfg->GetMacAddress();
    system_profile.play_coins =
        std::clamp(static_cast<int>(Service::PTM::Module::GetPlayCoins()), 0, 300);

    original_system_profile = system_profile;
    system_profile_loaded = true;
}

void ConfigureDialog::ApplySystemProfile() {
    if (!system_profile_loaded || !system_profile_editable || !system_profile_cfg) {
        return;
    }

    bool cfg_modified = false;
    const std::u16string username = NormalizeUsername(system_profile.username.data());
    const std::u16string original_username =
        NormalizeUsername(original_system_profile.username.data());
    if (username != original_username) {
        system_profile_cfg->SetUsername(username);
        cfg_modified = true;
    }
    if (system_profile.birth_month != original_system_profile.birth_month ||
        system_profile.birth_day != original_system_profile.birth_day) {
        system_profile_cfg->SetBirthday(static_cast<u8>(system_profile.birth_month),
                                        static_cast<u8>(system_profile.birth_day));
        cfg_modified = true;
    }
    if (system_profile.language != original_system_profile.language) {
        system_profile_cfg->SetSystemLanguage(
            static_cast<Service::CFG::SystemLanguage>(system_profile.language));
        cfg_modified = true;
    }
    if (system_profile.sound_mode != original_system_profile.sound_mode) {
        system_profile_cfg->SetSoundOutputMode(
            static_cast<Service::CFG::SoundOutputMode>(system_profile.sound_mode));
        cfg_modified = true;
    }
    if (system_profile.country != original_system_profile.country &&
        FindCountry(system_profile.country) != nullptr) {
        system_profile_cfg->SetCountryCode(static_cast<u8>(system_profile.country));
        cfg_modified = true;
    }
    if (system_profile.setup_needed != original_system_profile.setup_needed) {
        system_profile_cfg->SetSystemSetupNeeded(system_profile.setup_needed);
        cfg_modified = true;
    }
    if (system_profile.play_coins != original_system_profile.play_coins) {
        Service::PTM::Module::SetPlayCoins(static_cast<u16>(system_profile.play_coins));
    }

    if (cfg_modified) {
        const Result result = system_profile_cfg->UpdateConfigNANDSavegame();
        if (!result.IsSuccess()) {
            system_profile_status =
                fmt::format("Could not save the console profile (error 0x{:08X}).", result.raw);
            return;
        }
    }
    original_system_profile = system_profile;
}

void ConfigureDialog::RegenerateConsoleId() {
    if (!system_profile_editable || !system_profile_cfg) {
        return;
    }

    const auto [random_number, console_id] = system_profile_cfg->GenerateConsoleUniqueId();
    Result result = system_profile_cfg->SetConsoleUniqueId(random_number, console_id);
    if (result.IsSuccess()) {
        result = system_profile_cfg->UpdateConfigNANDSavegame();
    }
    if (!result.IsSuccess()) {
        system_profile_status =
            fmt::format("Could not regenerate the console ID (error 0x{:08X}).", result.raw);
        return;
    }

    system_profile.console_id = console_id;
    original_system_profile.console_id = console_id;
    system_profile_status = "Generated and saved a new console ID.";
}

void ConfigureDialog::RegenerateMacAddress() {
    if (!system_profile_editable || !system_profile_cfg) {
        return;
    }

    system_profile.mac_address = Service::CFG::GenerateRandomMAC();
    system_profile_cfg->GetMacAddress() = system_profile.mac_address;
    system_profile_cfg->SaveMacAddress();
    original_system_profile.mac_address = system_profile.mac_address;
    system_profile_status = "Generated and saved a new MAC address.";
}

void ConfigureDialog::DrawSystemProfile() {
    GroupHeader("Profile");
    if (!system_profile_loaded || !system_profile_cfg) {
        ImGui::TextDisabled("The console profile could not be loaded.");
        return;
    }

    if (!system_profile_editable) {
        ImGui::TextDisabled(
            "Profile and console identity changes are available after the running title is "
            "closed.");
    }

    ImGui::BeginDisabled(!system_profile_editable);
    if (GetVirtualKeyboard().InputText("Username", system_profile.username.data(),
                                       system_profile.username.size())) {
        // The OS keyboard limits bytes while CFG limits UTF-16 code units; normalize here.
        const std::u16string normalized = NormalizeUsername(system_profile.username.data());
        StoreUsername(system_profile.username, normalized);
    }
    Tooltip("The emulated 3DS username is limited to ten UTF-16 code units.");

    int month = system_profile.birth_month - 1;
    if (Combo("Birth month", month, kMonthItems)) {
        system_profile.birth_month = month + 1;
        system_profile.birth_day =
            std::min(system_profile.birth_day, kDaysInMonth[system_profile.birth_month - 1]);
    }
    ImGui::SliderInt("Birth day", &system_profile.birth_day, 1,
                     kDaysInMonth[system_profile.birth_month - 1]);
    Combo("System language", system_profile.language, kLanguageItems);
    Tooltip("Auto-select region may override the language when a title starts.");
    Combo("Sound output mode", system_profile.sound_mode, kSoundModeItems);

    const CountryOption* selected_country = FindCountry(system_profile.country);
    const std::string unknown_country =
        selected_country == nullptr ? fmt::format("Unknown ({})", system_profile.country) : "";
    const char* country_preview =
        selected_country != nullptr ? selected_country->name : unknown_country.c_str();
    if (ImGui::BeginCombo("Country", country_preview)) {
        for (const auto& country : kCountryItems) {
            const bool selected = system_profile.country == country.code;
            if (ImGui::Selectable(country.name, selected)) {
                system_profile.country = country.code;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Checkbox("Run System Setup when Home Menu is launched", &system_profile.setup_needed);
    ImGui::SliderInt("Play Coins", &system_profile.play_coins, 0, 300);
    ImGui::EndDisabled();

    const int configured_region = Settings::values.region_value.GetValue();
    if (configured_region != Settings::REGION_VALUE_AUTO_SELECT &&
        !Service::CFG::Module::IsValidRegionCountry(static_cast<u32>(configured_region),
                                                    static_cast<u8>(system_profile.country))) {
        ImGui::TextColored(ImVec4{1.0f, 0.35f, 0.35f, 1.0f},
                           "The selected country is not valid for the configured console region.");
    }

    GroupHeader("Console identity");
    ImGui::TextUnformatted(fmt::format("Console ID: 0x{:016X}", system_profile.console_id).c_str());
    ImGui::Text("MAC: %s", system_profile.mac_address.c_str());

    ImGui::BeginDisabled(!system_profile_editable);
    if (ImGui::Button("Regenerate console ID...")) {
        ImGui::OpenPopup("Regenerate console ID?");
    }
    ImGui::SameLine();
    if (ImGui::Button("Regenerate MAC address...")) {
        ImGui::OpenPopup("Regenerate MAC address?");
    }
    ImGui::EndDisabled();

    if (ImGui::BeginPopupModal("Regenerate console ID?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped(
            "This permanently replaces the current virtual 3DS console ID and can affect "
            "applications and network identity. Continue?");
        if (ImGui::Button("Regenerate##console_id", ImVec2(180.0f, 0.0f))) {
            RegenerateConsoleId();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##console_id", ImVec2(180.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Regenerate MAC address?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped(
            "This permanently replaces the current virtual 3DS MAC address. Do not continue if "
            "the address came from a real console unless you intend to replace it.");
        if (ImGui::Button("Regenerate##mac", ImVec2(180.0f, 0.0f))) {
            RegenerateMacAddress();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##mac", ImVec2(180.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }

    if (!system_profile_status.empty()) {
        ImGui::TextWrapped("%s", system_profile_status.c_str());
    }
}

void ConfigureDialog::DrawSystemTab() {
    auto& settings = Settings::values;

    GroupHeader("Console");
    DrawSwitchableSetting(per_game_mode, "is_new_3ds", settings.is_new_3ds, [&] {
        return SettingCheckbox("Enable New 3DS mode", settings.is_new_3ds);
    });
    DrawSwitchableSetting(per_game_mode, "required_online_lle",
                          settings.enable_required_online_lle_modules, [&] {
                              return SettingCheckbox("Enable required online LLE modules",
                                                     settings.enable_required_online_lle_modules);
                          });
    DrawSwitchableSetting(per_game_mode, "lle_applets", settings.lle_applets, [&] {
        return SettingCheckbox("Enable LLE applets", settings.lle_applets);
    });
    if (!per_game_mode) {
        SettingCheckbox("Apply region-free patch", settings.apply_region_free_patch);
    }

    // region_value uses -1 for auto-select, so it cannot be a plain combo index.
    DrawSwitchableSetting(per_game_mode, "region", settings.region_value, [&] {
        bool changed = false;
        int region = settings.region_value.GetValue();
        bool auto_region = region == Settings::REGION_VALUE_AUTO_SELECT;
        if (ImGui::Checkbox("Auto-select region", &auto_region)) {
            settings.region_value.SetValue(auto_region ? Settings::REGION_VALUE_AUTO_SELECT : 1);
            region = settings.region_value.GetValue();
            changed = true;
        }
        if (!auto_region && Combo("Console region", region, kRegionItems)) {
            settings.region_value.SetValue(region);
            changed = true;
        }
        return changed;
    });

    if (per_game_mode) {
        return;
    }

    DrawSystemProfile();

    GroupHeader("Clock");
    SettingEnumCombo("Clock source", settings.init_clock, kInitClockItems);
    if (settings.init_clock.GetValue() == Settings::InitClock::FixedTime) {
        // init_time is a Unix timestamp; there is no gamepad-friendly date picker.
        auto init_time = static_cast<int>(settings.init_time.GetValue());
        if (ImGui::InputInt("Start time (Unix)", &init_time)) {
            settings.init_time.SetValue(static_cast<u64>(std::max(0, init_time)));
        }
    }
    auto offset = static_cast<int>(settings.init_time_offset.GetValue());
    if (ImGui::InputInt("Clock offset (seconds)", &offset)) {
        settings.init_time_offset.SetValue(offset);
    }

    GroupHeader("System ticks");
    SettingEnumCombo("Initial ticks", settings.init_ticks_type, kInitTicksItems);
    if (settings.init_ticks_type.GetValue() == Settings::InitTicks::Fixed) {
        auto ticks = static_cast<int>(settings.init_ticks_override.GetValue());
        if (ImGui::InputInt("Tick override", &ticks)) {
            settings.init_ticks_override.SetValue(ticks);
        }
    }

    GroupHeader("Pedometer");
    int steps = static_cast<int>(settings.steps_per_hour.GetValue());
    if (ImGui::SliderInt("Steps per hour", &steps, 0, 65535)) {
        settings.steps_per_hour.SetValue(static_cast<u16>(steps));
    }

    GroupHeader("Plugin loader");
    SettingCheckbox("Enable 3GX plugin loader", settings.plugin_loader_enabled);
    SettingCheckbox("Allow games to change the plugin loader state", settings.allow_plugin_loader);
}

void ConfigureDialog::DrawGraphicsTab() {
    auto& settings = Settings::values;

    GroupHeader("API");
    settings.graphics_api.SetValue(Settings::GraphicsAPI::OpenGL);
    ImGui::TextUnformatted("Graphics API: OpenGL");
    ImGui::TextDisabled(
        "This package presents through OpenGL on Mesa/D3D12. Software and Vulkan do not have "
        "UWP presentation backends and are intentionally unavailable.");

    GroupHeader("Renderer");
    DrawSwitchableSetting(per_game_mode, "hw_shader", settings.use_hw_shader, [&] {
        return SettingCheckbox("Enable hardware shader", settings.use_hw_shader);
    });
    DrawSwitchableSetting(per_game_mode, "accurate_mul", settings.shaders_accurate_mul, [&] {
        return SettingCheckbox("Accurate multiplication", settings.shaders_accurate_mul);
    });
    if (!per_game_mode) {
        SettingCheckbox("Enable shader JIT", settings.use_shader_jit);
    }
    DrawSwitchableSetting(per_game_mode, "disk_shader_cache", settings.use_disk_shader_cache, [&] {
        return SettingCheckbox("Enable disk shader cache", settings.use_disk_shader_cache);
    });
    DrawSwitchableSetting(per_game_mode, "async_shader", settings.async_shader_compilation, [&] {
        return SettingCheckbox("Enable asynchronous shader compilation",
                               settings.async_shader_compilation);
    });
    DrawSwitchableSetting(per_game_mode, "async_presentation", settings.async_presentation, [&] {
        return SettingCheckbox("Enable asynchronous presentation", settings.async_presentation);
    });
    DrawSwitchableSetting(per_game_mode, "gpu_timings", settings.simulate_3ds_gpu_timings, [&] {
        return SettingCheckbox("Simulate 3DS GPU timings", settings.simulate_3ds_gpu_timings);
    });

    GroupHeader("Presentation");
    DrawSwitchableSetting(per_game_mode, "vsync", settings.use_vsync,
                          [&] { return SettingCheckbox("Enable VSync", settings.use_vsync); });
    ImGui::TextDisabled(
        "VSync synchronizes presentation to the display; emulation pacing remains independent.");
    Tooltip("The emulation worker keeps its frame limiter active because rendering and "
            "presentation use separate OpenGL contexts. Frame and turbo limits still apply.");
    DrawSwitchableSetting(
        per_game_mode, "skip_duplicates", settings.use_skip_duplicate_frames, [&] {
            return SettingCheckbox("Skip duplicate frames", settings.use_skip_duplicate_frames);
        });
    DrawSwitchableSetting(per_game_mode, "refresh_detection",
                          settings.use_display_refresh_rate_detection, [&] {
                              return SettingCheckbox("Use display refresh rate detection",
                                                     settings.use_display_refresh_rate_detection);
                          });
    DrawSwitchableSetting(per_game_mode, "render_delay", settings.delay_game_render_thread_us, [&] {
        auto delay = static_cast<int>(settings.delay_game_render_thread_us.GetValue());
        const bool changed = ImGui::SliderInt("Render thread delay (us)", &delay, 0, 16000);
        if (changed) {
            settings.delay_game_render_thread_us.SetValue(static_cast<u16>(delay));
        }
        return changed;
    });
}

void ConfigureDialog::DrawEnhancementsTab() {
    auto& settings = Settings::values;

    GroupHeader("Internal resolution");
    DrawSwitchableSetting(per_game_mode, "resolution", settings.resolution_factor, [&] {
        auto resolution = static_cast<int>(settings.resolution_factor.GetValue());
        const bool changed = Combo("Resolution scale", resolution, kResolutionItems);
        if (changed) {
            settings.resolution_factor.SetValue(static_cast<u32>(resolution));
        }
        return changed;
    });
    Tooltip("Auto matches the current Xbox output surface, as in the Windows build.");
    DrawSwitchableSetting(per_game_mode, "integer_scaling", settings.use_integer_scaling, [&] {
        return SettingCheckbox("Use integer scaling", settings.use_integer_scaling);
    });
    DrawSwitchableSetting(per_game_mode, "linear_filtering", settings.filter_mode, [&] {
        return SettingCheckbox("Enable linear filtering", settings.filter_mode);
    });

    GroupHeader("Post-processing");
    DrawSwitchableSetting(per_game_mode, "texture_filter", settings.texture_filter, [&] {
        return SettingEnumCombo("Texture filter", settings.texture_filter, kTextureFilterItems);
    });
    DrawSwitchableSetting(per_game_mode, "texture_sampling", settings.texture_sampling, [&] {
        return SettingEnumCombo("Texture sampling", settings.texture_sampling,
                                kTextureSamplingItems);
    });

    GroupHeader("Stereoscopy");
    DrawSwitchableSetting(per_game_mode, "render_3d", settings.render_3d, [&] {
        return SettingEnumCombo("Stereoscopic 3D mode", settings.render_3d, kStereoItems);
    });
    DrawSwitchableSetting(per_game_mode, "factor_3d", settings.factor_3d, [&] {
        auto factor = static_cast<int>(settings.factor_3d.GetValue());
        const bool changed = ImGui::SliderInt("Depth", &factor, 0, 100, "%d%%");
        if (changed) {
            settings.factor_3d.SetValue(static_cast<u32>(factor));
        }
        return changed;
    });
    if (!per_game_mode) {
        SettingEnumCombo("Eye to render in monoscopic mode", settings.mono_render_option,
                         kMonoItems);
    }
    DrawSwitchableSetting(per_game_mode, "swap_eyes", settings.swap_eyes_3d,
                          [&] { return SettingCheckbox("Swap eyes", settings.swap_eyes_3d); });
    DrawSwitchableSetting(per_game_mode, "disable_right_eye", settings.disable_right_eye_render,
                          [&] {
                              return SettingCheckbox("Disable right eye rendering",
                                                     settings.disable_right_eye_render);
                          });

    GroupHeader("Custom textures");
    DrawSwitchableSetting(per_game_mode, "custom_textures", settings.custom_textures, [&] {
        return SettingCheckbox("Use custom textures", settings.custom_textures);
    });
    DrawSwitchableSetting(per_game_mode, "preload_textures", settings.preload_textures, [&] {
        return SettingCheckbox("Preload custom textures", settings.preload_textures);
    });
    DrawSwitchableSetting(per_game_mode, "async_custom_textures", settings.async_custom_loading,
                          [&] {
                              return SettingCheckbox("Load custom textures asynchronously",
                                                     settings.async_custom_loading);
                          });
    DrawSwitchableSetting(per_game_mode, "dump_textures", settings.dump_textures,
                          [&] { return SettingCheckbox("Dump textures", settings.dump_textures); });
}

void ConfigureDialog::DrawLayoutTab() {
    auto& settings = Settings::values;

    GroupHeader("Screen layout");
    DrawSwitchableSetting(per_game_mode, "layout", settings.layout_option, [&] {
        int layout_index = 0;
        for (std::size_t i = 0; i < kLayoutValues.size(); ++i) {
            if (settings.layout_option.GetValue() == kLayoutValues[i]) {
                layout_index = static_cast<int>(i);
                break;
            }
        }
        const bool changed = Combo("Layout", layout_index, kLayoutItems,
                                   "Separate windows is unavailable in an Xbox app container.");
        if (changed) {
            settings.layout_option.SetValue(kLayoutValues[static_cast<std::size_t>(layout_index)]);
        }
        return changed;
    });
    DrawSwitchableSetting(
        per_game_mode, "small_screen_position", settings.small_screen_position, [&] {
            return SettingEnumCombo("Small screen position", settings.small_screen_position,
                                    kSmallScreenPosItems);
        });
    DrawSwitchableSetting(per_game_mode, "swap_screen", settings.swap_screen,
                          [&] { return SettingCheckbox("Swap screens", settings.swap_screen); });
    DrawSwitchableSetting(per_game_mode, "upright", settings.upright_screen, [&] {
        return SettingCheckbox("Upright orientation", settings.upright_screen);
    });

    DrawSwitchableSetting(
        per_game_mode, "large_screen_proportion", settings.large_screen_proportion, [&] {
            return SettingSlider("Large screen proportion", settings.large_screen_proportion, 1.0,
                                 16.0, "%.1f");
        });

    GroupHeader("Spacing");
    DrawSwitchableSetting(per_game_mode, "screen_gap", settings.screen_gap, [&] {
        return SettingSlider("Screen gap", settings.screen_gap, 0.0, 200.0);
    });

    if (!per_game_mode) {
        GroupHeader("Top screen padding");
        SettingCheckbox("Stretch top screen", settings.screen_top_stretch);
        auto top_lr = static_cast<int>(settings.screen_top_leftright_padding.GetValue());
        if (ImGui::SliderInt("Top left/right padding", &top_lr, 0, 500)) {
            settings.screen_top_leftright_padding.SetValue(static_cast<u16>(top_lr));
        }
        auto top_tb = static_cast<int>(settings.screen_top_topbottom_padding.GetValue());
        if (ImGui::SliderInt("Top top/bottom padding", &top_tb, 0, 500)) {
            settings.screen_top_topbottom_padding.SetValue(static_cast<u16>(top_tb));
        }

        GroupHeader("Bottom screen padding");
        SettingCheckbox("Stretch bottom screen", settings.screen_bottom_stretch);
        auto bottom_lr = static_cast<int>(settings.screen_bottom_leftright_padding.GetValue());
        if (ImGui::SliderInt("Bottom left/right padding", &bottom_lr, 0, 500)) {
            settings.screen_bottom_leftright_padding.SetValue(static_cast<u16>(bottom_lr));
        }
        auto bottom_tb = static_cast<int>(settings.screen_bottom_topbottom_padding.GetValue());
        if (ImGui::SliderInt("Bottom top/bottom padding", &bottom_tb, 0, 500)) {
            settings.screen_bottom_topbottom_padding.SetValue(static_cast<u16>(bottom_tb));
        }

        if (settings.layout_option.GetValue() == Settings::LayoutOption::CustomLayout) {
            GroupHeader("Custom layout");
            const auto rect = [](const char* label, auto& x, auto& y, auto& w, auto& h) {
                int values_arr[4]{static_cast<int>(x.GetValue()), static_cast<int>(y.GetValue()),
                                  static_cast<int>(w.GetValue()), static_cast<int>(h.GetValue())};
                if (ImGui::InputInt4(label, values_arr)) {
                    x.SetValue(static_cast<u16>(std::max(0, values_arr[0])));
                    y.SetValue(static_cast<u16>(std::max(0, values_arr[1])));
                    w.SetValue(static_cast<u16>(std::max(0, values_arr[2])));
                    h.SetValue(static_cast<u16>(std::max(0, values_arr[3])));
                }
            };
            rect("Top screen (x, y, w, h)", settings.custom_top_x, settings.custom_top_y,
                 settings.custom_top_width, settings.custom_top_height);
            rect("Bottom screen (x, y, w, h)", settings.custom_bottom_x, settings.custom_bottom_y,
                 settings.custom_bottom_width, settings.custom_bottom_height);

            auto opacity = static_cast<int>(settings.custom_second_layer_opacity.GetValue());
            if (ImGui::SliderInt("Second layer opacity", &opacity, 0, 100, "%d%%")) {
                settings.custom_second_layer_opacity.SetValue(static_cast<u16>(opacity));
            }
        }
    } else if (settings.layout_option.GetValue() == Settings::LayoutOption::CustomLayout) {
        ImGui::TextDisabled("Custom screen rectangles use the global layout coordinates.");
    }

    GroupHeader("Background colour");
    bool override_background = !settings.bg_red.UsingGlobal() || !settings.bg_green.UsingGlobal() ||
                               !settings.bg_blue.UsingGlobal();
    if (per_game_mode) {
        if (ImGui::Checkbox("Override global##background", &override_background)) {
            const float red = settings.bg_red.GetValue(true);
            const float green = settings.bg_green.GetValue(true);
            const float blue = settings.bg_blue.GetValue(true);
            settings.bg_red.SetGlobal(!override_background);
            settings.bg_green.SetGlobal(!override_background);
            settings.bg_blue.SetGlobal(!override_background);
            if (override_background) {
                settings.bg_red.SetValue(red);
                settings.bg_green.SetValue(green);
                settings.bg_blue.SetValue(blue);
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!override_background);
    } else {
        ImGui::BeginDisabled(override_background);
    }
    float colour[3]{settings.bg_red.GetValue(), settings.bg_green.GetValue(),
                    settings.bg_blue.GetValue()};
    if (ImGui::ColorEdit3("Background", colour)) {
        settings.bg_red.SetValue(colour[0]);
        settings.bg_green.SetValue(colour[1]);
        settings.bg_blue.SetValue(colour[2]);
    }
    ImGui::EndDisabled();
}

void ConfigureDialog::DrawAudioTab() {
    auto& settings = Settings::values;

    GroupHeader("Output");
    DrawSwitchableSetting(per_game_mode, "audio_emulation", settings.audio_emulation, [&] {
        return SettingEnumCombo("Emulation", settings.audio_emulation, kAudioEmulationItems);
    });
    DrawSwitchableSetting(per_game_mode, "audio_stretch", settings.enable_audio_stretching, [&] {
        return SettingCheckbox("Enable audio stretching", settings.enable_audio_stretching,
                               "Reduces audio glitches when emulation runs below full speed, at "
                               "the cost of latency.");
    });
    DrawSwitchableSetting(per_game_mode, "realtime_audio", settings.enable_realtime_audio, [&] {
        return SettingCheckbox("Enable realtime audio", settings.enable_realtime_audio);
    });
    DrawSwitchableSetting(per_game_mode, "headphones", settings.simulate_headphones_plugged, [&] {
        return SettingCheckbox("Simulate headphones being plugged in",
                               settings.simulate_headphones_plugged);
    });
    DrawSwitchableSetting(per_game_mode, "volume", settings.volume, [&] {
        auto volume = settings.volume.GetValue();
        const bool changed = ImGui::SliderFloat("Volume", &volume, 0.0f, 1.0f, "%.2f");
        if (changed) {
            settings.volume.SetValue(volume);
        }
        return changed;
    });

    if (!per_game_mode) {
        GroupHeader("Microphone");

        const auto input_types = AudioCore::ListInputs();
        const auto current_type = settings.input_type.GetValue();
        const auto& effective_type = AudioCore::GetInputDetails(current_type);
        const char* type_preview =
            current_type == AudioCore::InputType::Auto ? "Auto" : effective_type.name.data();
        if (ImGui::BeginCombo("Input type", type_preview)) {
            const bool auto_selected = current_type == AudioCore::InputType::Auto;
            if (ImGui::Selectable("Auto", auto_selected)) {
                settings.input_type.SetValue(AudioCore::InputType::Auto);
                microphone_devices_type = -1;
            }
            if (auto_selected) {
                ImGui::SetItemDefaultFocus();
            }
            for (const auto& input : input_types) {
                const bool selected = current_type == input.type;
                if (ImGui::Selectable(input.name.data(), selected)) {
                    settings.input_type.SetValue(input.type);
                    microphone_devices_type = -1;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        Tooltip("Auto chooses the first real capture backend available. UWP uses SDL2/WASAPI.");

        const auto selected_type = settings.input_type.GetValue();
        const auto& selected_details = AudioCore::GetInputDetails(selected_type);
        const int effective_type_value = static_cast<int>(selected_details.type);
        if (microphone_devices_type != effective_type_value) {
            microphone_devices.clear();
            microphone_devices.emplace_back(AudioCore::auto_device_name);
            if (selected_details.real) {
                for (auto& device : selected_details.list_devices()) {
                    if (!device.empty() &&
                        std::find(microphone_devices.begin(), microphone_devices.end(), device) ==
                            microphone_devices.end()) {
                        microphone_devices.push_back(std::move(device));
                    }
                }
            }
            microphone_devices_type = effective_type_value;
        }

        std::string selected_device = settings.input_device.GetValue();
        if (selected_device.empty()) {
            selected_device = AudioCore::auto_device_name;
        }
        if (ImGui::BeginCombo("Input device", selected_device.c_str())) {
            for (const auto& device : microphone_devices) {
                const bool selected = selected_device == device;
                if (ImGui::Selectable(device.c_str(), selected)) {
                    settings.input_device.SetValue(device);
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled(
            "Headset microphone capture is available when Xbox grants the app permission.");
    }
}

void ConfigureDialog::DrawDebugTab() {
    auto& settings = Settings::values;

    GroupHeader("CPU");
    if (!per_game_mode) {
        SettingCheckbox("Enable CPU JIT", settings.use_cpu_jit,
                        "Disabling falls back to the interpreter, which is dramatically slower.");
    }
    DrawSwitchableSetting(per_game_mode, "cpu_clock", settings.cpu_clock_percentage, [&] {
        auto clock = settings.cpu_clock_percentage.GetValue();
        const bool changed = ImGui::SliderInt("CPU clock speed", &clock, 5, 400, "%d%%");
        if (changed) {
            settings.cpu_clock_percentage.SetValue(clock);
        }
        return changed;
    });

    if (per_game_mode) {
        ImGui::TextDisabled("CPU JIT and debugging services are global frontend settings.");
        return;
    }

    GroupHeader("Logging");
    SettingInputText("Log filter", settings.log_filter);
    SettingInputText("Log regex filter", settings.log_regex_filter);
    SettingCheckbox("Flush log entries immediately", settings.instant_debug_log,
                    "Writes each entry as it is produced instead of buffering. Slower, but "
                    "nothing is lost if the process is terminated.");

    GroupHeader("Graphics debugging");
    SettingCheckbox("Enable renderer debug layer", settings.renderer_debug);
    SettingCheckbox("Enable Pica debugging", settings.pica_debugging);
    SettingCheckbox("Dump command buffers", settings.dump_command_buffers);

    GroupHeader("Emulation");
    SettingCheckbox("Enable exception handler", settings.enable_exception_handler);
    SettingCheckbox("Deterministic async operations", settings.deterministic_async_operations);
    SettingCheckbox("Delay start for LLE modules", settings.delay_start_for_lle_modules);
    SettingCheckbox("Use unique console type data", settings.toggle_unique_data_console_type);

    GroupHeader("LLE service modules");
    ImGui::TextDisabled("Per-module selections are saved globally and take effect on the next "
                        "application launch.");
    if (ImGui::BeginTable("##lle_service_modules", 3,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        std::size_t index = 0;
        for (const auto& service_module : Service::service_module_map) {
            if ((index % 3) == 0) {
                ImGui::TableNextRow();
            }
            ImGui::TableSetColumnIndex(static_cast<int>(index % 3));
            ImGui::PushID(service_module.name.c_str());
            bool& use_lle = settings.lle_modules[service_module.name];
            ImGui::Checkbox(service_module.name.c_str(), &use_lle);
            if (service_module.is_online_recommended) {
                Tooltip("Upstream recommends this module for online functionality.");
            }
            ImGui::PopID();
            ++index;
        }
        ImGui::EndTable();
    }

    GroupHeader("Remote debugging");
    SettingCheckbox("Enable GDB stub", settings.use_gdbstub,
                    "Halts at startup until a GDB client attaches.");
    if (settings.use_gdbstub.GetValue()) {
        SettingSlider("GDB port", settings.gdbstub_port, 1024.0, 65535.0);
    }
    SettingCheckbox("Enable RPC server", settings.enable_rpc_server,
                    "Exposes the scripting interface over the network.");
}

void ConfigureDialog::DrawStorageTab() {
    auto& settings = Settings::values;

    GroupHeader("Storage behaviour");
    SettingCheckbox("Use virtual SD card", settings.use_virtual_sd);
    SettingCheckbox("Override NAND and SDMC subfolders", settings.use_custom_storage);
    SettingCheckbox("Compress CIA installs", settings.compress_cia_installs);
    SettingCheckbox("Asynchronous filesystem operations", settings.async_fs_operations);

    struct PathChoice {
        std::string_view path;
        bool recommended;
    };
    static std::string storage_location_status;
    if (auto custom = TakeCustomStorageResult()) {
        if (!custom->error.empty()) {
            storage_location_status = std::move(custom->error);
        } else if (custom->path.empty()) {
            storage_location_status = "Custom folder selection cancelled.";
        } else {
            std::string error;
            if (!Storage::EnsureWritableDirectory(custom->path, error)) {
                storage_location_status = "Could not use " + custom->path + ": " + std::move(error);
            } else {
                switch (custom->target) {
                case CustomStorageTarget::Applications:
                    UWP::values.game_directory.SetValue(custom->path);
                    break;
                case CustomStorageTarget::Nand:
                    FileUtil::UpdateUserPath(FileUtil::UserPath::NANDDir, custom->path);
                    settings.use_custom_storage.SetValue(true);
                    break;
                case CustomStorageTarget::Sdmc:
                    FileUtil::UpdateUserPath(FileUtil::UserPath::SDMCDir, custom->path);
                    settings.use_custom_storage.SetValue(true);
                    break;
                case CustomStorageTarget::TextureLoad:
                    FileUtil::UpdateUserPath(FileUtil::UserPath::LoadDir, custom->path);
                    break;
                case CustomStorageTarget::Mods:
                    FileUtil::UpdateUserPath(FileUtil::UserPath::ModsDir, custom->path);
                    break;
                case CustomStorageTarget::TextureDump:
                    FileUtil::UpdateUserPath(FileUtil::UserPath::DumpDir, custom->path);
                    break;
                case CustomStorageTarget::Cheats:
                    FileUtil::UpdateUserPath(FileUtil::UserPath::CheatsDir, custom->path);
                    break;
                case CustomStorageTarget::ScreenScraper:
                    UWP::values.screenscraper_storage.SetValue(custom->path);
                    break;
                }
                storage_location_status = "Selected and prepared " + custom->path + ".";
            }
        }
    }
    const auto draw_path_dropdown = [](const char* label, const std::string& current,
                                       const std::array<PathChoice, 2>& choices,
                                       CustomStorageTarget custom_target, const auto& select) {
        const auto display_path = [](std::string path) {
            std::replace(path.begin(), path.end(), '\\', '/');
            return path;
        };
        const std::string preview = display_path(current);
        ImGui::PushID(label);
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##path", preview.c_str())) {
            const auto normalized = [](std::string path) {
                std::replace(path.begin(), path.end(), '\\', '/');
                while (!path.empty() && (path.back() == '\\' || path.back() == '/')) {
                    path.pop_back();
                }
                return Common::ToLower(path);
            };
            bool preset_selected = false;
            for (const auto& choice : choices) {
                const std::string path{choice.path};
                const bool selected = normalized(current) == normalized(path);
                preset_selected |= selected;
                const std::string option =
                    display_path(path) + (choice.recommended ? " (Recommended)" : "");
                const bool available = Storage::IsPathDriveAvailable(path);
                ImGui::BeginDisabled(!available);
                if (ImGui::Selectable(option.c_str(), selected)) {
                    std::string error;
                    if (Storage::EnsureWritableDirectory(path, error)) {
                        select(path.c_str());
                        storage_location_status =
                            "Selected and prepared " + display_path(path) + ".";
                    } else {
                        storage_location_status = "Could not use " + path + ": " + error;
                    }
                }
                ImGui::EndDisabled();
                if (!available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Drive is not connected");
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            if (ImGui::Selectable("Custom", !preset_selected)) {
                if (StartCustomStoragePicker(custom_target)) {
                    storage_location_status = "Choose a custom folder...";
                } else {
                    storage_location_status = "A folder picker is already open.";
                }
            }
            if (!preset_selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
        ImGui::Spacing();
    };

    constexpr std::array<PathChoice, 2> application_paths{{
        {Storage::DevelopmentApplicationsPath, false},
        {Storage::ExternalApplicationsPath, true},
    }};
    constexpr std::array<PathChoice, 2> nand_paths{{
        {Storage::DevelopmentNandPath, false},
        {Storage::ExternalNandPath, true},
    }};
    constexpr std::array<PathChoice, 2> sdmc_paths{{
        {Storage::DevelopmentSdmcPath, false},
        {Storage::ExternalSdmcPath, true},
    }};
    constexpr std::array<PathChoice, 2> texture_load_paths{{
        {Storage::DevelopmentTextureLoadPath, false},
        {Storage::ExternalTextureLoadPath, true},
    }};
    constexpr std::array<PathChoice, 2> mod_paths{{
        {Storage::DevelopmentModsPath, false},
        {Storage::ExternalModsPath, true},
    }};
    constexpr std::array<PathChoice, 2> texture_dump_paths{{
        {Storage::DevelopmentTextureDumpPath, false},
        {Storage::ExternalTextureDumpPath, true},
    }};
    constexpr std::array<PathChoice, 2> cheat_paths{{
        {Storage::DevelopmentCheatsPath, false},
        {Storage::ExternalCheatsPath, true},
    }};
    constexpr std::array<PathChoice, 2> screenscraper_paths{{
        {Storage::DevelopmentScreenScraperPath, false},
        {Storage::ExternalScreenScraperPath, true},
    }};

    GroupHeader("Storage locations");
    draw_path_dropdown("Applications", UWP::values.game_directory.GetValue(), application_paths,
                       CustomStorageTarget::Applications,
                       [](const char* path) { UWP::values.game_directory.SetValue(path); });
    SettingCheckbox("Search subdirectories", UWP::values.game_directory_deep_scan);

    draw_path_dropdown("NAND", FileUtil::GetUserPath(FileUtil::UserPath::NANDDir), nand_paths,
                       CustomStorageTarget::Nand, [&settings](const char* path) {
                           FileUtil::UpdateUserPath(FileUtil::UserPath::NANDDir, path);
                           settings.use_custom_storage.SetValue(true);
                       });
    draw_path_dropdown("SDMC", FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir), sdmc_paths,
                       CustomStorageTarget::Sdmc, [&settings](const char* path) {
                           FileUtil::UpdateUserPath(FileUtil::UserPath::SDMCDir, path);
                           settings.use_custom_storage.SetValue(true);
                       });
    draw_path_dropdown("Texture Load", FileUtil::GetUserPath(FileUtil::UserPath::LoadDir),
                       texture_load_paths, CustomStorageTarget::TextureLoad, [](const char* path) {
                           FileUtil::UpdateUserPath(FileUtil::UserPath::LoadDir, path);
                       });
    draw_path_dropdown("Texture Dump", FileUtil::GetUserPath(FileUtil::UserPath::DumpDir),
                       texture_dump_paths, CustomStorageTarget::TextureDump, [](const char* path) {
                           FileUtil::UpdateUserPath(FileUtil::UserPath::DumpDir, path);
                       });
    draw_path_dropdown("Mods", FileUtil::GetUserPath(FileUtil::UserPath::ModsDir), mod_paths,
                       CustomStorageTarget::Mods, [](const char* path) {
                           FileUtil::UpdateUserPath(FileUtil::UserPath::ModsDir, path);
                       });
    draw_path_dropdown("Cheats", FileUtil::GetUserPath(FileUtil::UserPath::CheatsDir), cheat_paths,
                       CustomStorageTarget::Cheats, [](const char* path) {
                           FileUtil::UpdateUserPath(FileUtil::UserPath::CheatsDir, path);
                       });
    draw_path_dropdown("ScreenScraper Storage", UWP::values.screenscraper_storage.GetValue(),
                       screenscraper_paths, CustomStorageTarget::ScreenScraper,
                       [](const char* path) { UWP::values.screenscraper_storage.SetValue(path); });
    if (!storage_location_status.empty()) {
        ImGui::TextWrapped("%s", storage_location_status.c_str());
    }
}

void ConfigureDialog::StartPolling(bool analog) {
    // Stop anything already armed, so switching slots mid-capture cannot leave a poller running.
    StopPolling();

    pollers = InputCommon::Polling::GetPollers(analog ? InputCommon::Polling::DeviceType::Analog
                                                      : InputCommon::Polling::DeviceType::Button);
    for (auto& poller : pollers) {
        poller->Start();
    }
    binding_capture_popup_pending = !pollers.empty();
}

void ConfigureDialog::StopPolling() {
    for (auto& poller : pollers) {
        poller->Stop();
    }
    pollers.clear();
    key_capture = KeyCaptureTarget::None;
    key_capture_index = -1;
    KeyboardInput::SetKeyCaptureActive(false);
}

void ConfigureDialog::StartKeyCapture(KeyCaptureTarget target, int index) {
    StopPolling();
    polling_button = -1;
    polling_analog = -1;
    polling_hotkey = -1;
    polling_menu_chord = -1;
    polling_auto_map = false;
    key_capture = target;
    key_capture_index = index;
    key_capture_step = 0;
    if (target == KeyCaptureTarget::Analog) {
        key_capture_analog = KeyboardInput::AnalogKeysOf(
            Settings::values.current_input_profile.analogs[static_cast<std::size_t>(index)]);
    }
    KeyboardInput::SetKeyCaptureActive(true);
    binding_capture_popup_pending = true;
}

void ConfigureDialog::UpdateKeyCapture() {
    if (key_capture == KeyCaptureTarget::None) {
        return;
    }
    auto& profile = Settings::values.current_input_profile;
    const auto index = static_cast<std::size_t>(key_capture_index);
    for (const auto& press : KeyboardInput::KeyPresses()) {
        if (KeyboardInput::IsModifierKey(press.key)) {
            continue;
        }
        // As in the desktop frontend, Escape cancels; Backspace clears the key.
        if (press.key == SDLK_ESCAPE) {
            StopPolling();
            return;
        }
        const s32 key = press.key == SDLK_BACKSPACE ? 0 : press.key;
        switch (key_capture) {
        case KeyCaptureTarget::Button:
            KeyboardInput::SetButtonKey(profile.buttons[index], key);
            break;
        case KeyCaptureTarget::Hotkey:
            UWP::values.keyboard_hotkeys[index] =
                key == 0 ? std::string{} : KeyboardInput::ChordText(press);
            break;
        case KeyCaptureTarget::Analog:
            if (key == 0) {
                KeyboardInput::SetAnalogKeys(profile.analogs[index], {});
                break;
            }
            key_capture_analog.keys[key_capture_step++] = key;
            if (key_capture_step < key_capture_analog.keys.size()) {
                continue;
            }
            KeyboardInput::SetAnalogKeys(profile.analogs[index], key_capture_analog);
            break;
        default:
            break;
        }
        StopPolling();
        return;
    }
}

bool ConfigureDialog::IsBindingCaptureActive() const {
    return polling_button >= 0 || polling_analog >= 0 || polling_hotkey >= 0 ||
           polling_menu_chord >= 0 || polling_auto_map || key_capture != KeyCaptureTarget::None;
}

void ConfigureDialog::DrawBindingCapturePopup() {
    constexpr const char* popup_name = "Waiting for input";
    if (binding_capture_popup_pending) {
        ImGui::OpenPopup(popup_name);
        binding_capture_popup_pending = false;
    }

    if (!ImGui::BeginPopupModal(popup_name, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    if (!IsBindingCaptureActive()) {
        // Closing here keeps the same press inside the modal so it cannot activate the control
        // beneath.
        ImGui::CloseCurrentPopup();
    } else if (key_capture == KeyCaptureTarget::Analog) {
        static constexpr std::array directions{"up", "down", "left", "right", "modifier"};
        ImGui::Text("Press the key for %s.", directions[key_capture_step]);
    } else if (key_capture == KeyCaptureTarget::Hotkey) {
        ImGui::TextUnformatted("Press the key combination to use for this command.");
    } else if (key_capture == KeyCaptureTarget::Button) {
        ImGui::TextUnformatted("Press the key to use for this control.");
    } else if (polling_analog >= 0) {
        ImGui::TextUnformatted("Move the stick to use for this control.");
    } else if (polling_auto_map) {
        ImGui::TextUnformatted("Press the right-hand face button on the controller.");
    } else {
        ImGui::TextUnformatted("Press the button to use for this control.");
    }
    if (IsBindingCaptureActive()) {
        ImGui::Spacing();
        ImGui::TextDisabled(key_capture != KeyCaptureTarget::None
                                ? "Esc cancels. Backspace clears the key."
                                : "This input will not operate the settings page underneath.");
    }
    ImGui::EndPopup();
}

void ConfigureDialog::ApplyMappingScope() {
    auto& profile = Settings::values.current_input_profile;
    const char* maptype = "guid+port";
    switch (profile.maptype) {
    case Settings::InputMappingType::AllControllers:
        maptype = "all";
        break;
    case Settings::InputMappingType::Guid:
        maptype = "guid";
        break;
    case Settings::InputMappingType::GuidPort:
        break;
    }

    const auto update = [maptype](std::string& binding, bool button) {
        Common::ParamPackage params{KeyboardInput::ButtonBinding(binding)};
        if (params.Get("engine", "") == "sdl") {
            params.Set("maptype", maptype);
            if (button) {
                KeyboardInput::SetButtonBinding(binding, params.Serialize());
            } else {
                KeyboardInput::SetAnalogBinding(binding, params.Serialize());
            }
        }
    };
    for (auto& binding : profile.buttons) {
        update(binding, true);
    }
    for (auto& binding : profile.analogs) {
        update(binding, false);
    }
}

bool ConfigureDialog::MapController(const Common::ParamPackage& controller) {
    if (controller.Get("engine", "") != "sdl" || controller.Get("api", "") != "controller") {
        return false;
    }

    bool mapped = false;
    auto& profile = Settings::values.current_input_profile;
    decltype(profile.buttons) buttons{};
    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        Common::ParamPackage binding = InputCommon::GetControllerButtonBinds(controller, i);
        if (binding.Has("engine")) {
            buttons[i] = binding.Serialize();
        }
    }
    // InputCommon's XInput preset is Nintendo-positioned; swap the face-button pairs to match
    // labels.
    std::swap(buttons[Settings::NativeButton::A], buttons[Settings::NativeButton::B]);
    std::swap(buttons[Settings::NativeButton::X], buttons[Settings::NativeButton::Y]);
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        if (!buttons[i].empty()) {
            KeyboardInput::SetButtonBinding(profile.buttons[i], buttons[i]);
            mapped = true;
        }
    }
    for (int i = 0; i < Settings::NativeAnalog::NumAnalogs; ++i) {
        Common::ParamPackage binding = InputCommon::GetControllerAnalogBinds(controller, i);
        if (binding.Has("engine")) {
            if (!binding.Has("deadzone")) {
                binding.Set("deadzone", 0.1f);
            }
            KeyboardInput::SetAnalogBinding(profile.analogs[i], binding.Serialize());
            mapped = true;
        }
    }
    ApplyMappingScope();
    return mapped;
}

void ConfigureDialog::ReloadDsu() {
    UWP::NormalizeInputSettings();
    auto& settings = Settings::values;
    if (!settings.input_profiles.empty() && settings.current_input_profile_index >= 0 &&
        settings.current_input_profile_index < static_cast<int>(settings.input_profiles.size())) {
        Settings::SaveProfile(settings.current_input_profile_index);
    }
    InputCommon::ReloadInputDevices();
    dsu_async->test_result.store(0);
}

void ConfigureDialog::UpdatePolling() {
    if (pollers.empty()) {
        return;
    }

    // Poll every backend each frame; the first one to produce a binding wins.
    for (auto& poller : pollers) {
        const Common::ParamPackage params = poller->GetNextInput();
        const std::string engine = params.Get("engine", "");
        const bool xbox_controller = engine == "sdl" && params.Get("api", "") == "controller";
        const bool dsu_input = engine == "cemuhookudp";
        if (!params.Has("engine") || (!xbox_controller && !dsu_input)) {
            continue;
        }

        auto& profile = Settings::values.current_input_profile;
        if (polling_auto_map) {
            if (!MapController(params)) {
                continue;
            }
        } else if (polling_analog >= 0) {
            KeyboardInput::SetAnalogBinding(
                profile.analogs[static_cast<std::size_t>(polling_analog)], params.Serialize());
        } else if (polling_button >= 0) {
            KeyboardInput::SetButtonBinding(
                profile.buttons[static_cast<std::size_t>(polling_button)], params.Serialize());
        } else if (polling_hotkey >= 0) {
            UWP::values.controller_hotkeys[static_cast<std::size_t>(polling_hotkey)] =
                params.Serialize();
        } else if (polling_menu_chord >= 0) {
            UWP::values.in_game_menu_chord[static_cast<std::size_t>(polling_menu_chord)] =
                params.Serialize();
        }

        polling_button = -1;
        polling_analog = -1;
        polling_hotkey = -1;
        polling_menu_chord = -1;
        polling_auto_map = false;
        ApplyMappingScope();
        StopPolling();
        return;
    }
}

void ConfigureDialog::DrawInputTab() {
    UWP::NormalizeInputSettings();
    auto& profile = Settings::values.current_input_profile;
    auto& profiles = Settings::values.input_profiles;

    GroupHeader("Profile");
    const char* current_name = profile.name.empty() ? "Default" : profile.name.c_str();
    if (ImGui::BeginCombo("Active profile", current_name)) {
        for (std::size_t i = 0; i < profiles.size(); ++i) {
            const bool selected =
                static_cast<int>(i) == Settings::values.current_input_profile_index;
            const std::string label =
                profiles[i].name.empty() ? fmt::format("Profile {}", i + 1) : profiles[i].name;
            if (ImGui::Selectable(label.c_str(), selected)) {
                Settings::SaveProfile(Settings::values.current_input_profile_index);
                Settings::LoadProfile(static_cast<int>(i));
                UWP::NormalizeInputSettings();
                ReloadDsu();
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    std::array<char, 96> profile_name{};
    const auto profile_name_length = std::min(profile.name.size(), profile_name.size() - 1);
    std::copy_n(profile.name.begin(), profile_name_length, profile_name.begin());
    if (GetVirtualKeyboard().InputText("Profile name", profile_name.data(), profile_name.size())) {
        const std::string new_name{profile_name.data()};
        if (!new_name.empty()) {
            Settings::RenameCurrentProfile(new_name);
            Settings::SaveProfile(Settings::values.current_input_profile_index);
        }
    }
    if (ImGui::Button("New profile")) {
        Settings::SaveProfile(Settings::values.current_input_profile_index);
        Settings::CreateProfile(fmt::format("Profile {}", profiles.size() + 1));
        UWP::NormalizeInputSettings();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(profiles.size() <= 1);
    if (ImGui::Button("Delete profile")) {
        Settings::DeleteProfile(Settings::values.current_input_profile_index);
        UWP::NormalizeInputSettings();
        ReloadDsu();
    }
    ImGui::EndDisabled();

    static constexpr std::array kMappingScopeItems{"All Xbox controllers", "This controller (GUID)",
                                                   "This controller and port"};
    int mapping_scope = static_cast<int>(profile.maptype);
    if (Combo("Mapping scope", mapping_scope, kMappingScopeItems,
              "Controls whether bindings follow every Xbox pad, a particular controller model, "
              "or one controller port.")) {
        profile.maptype = static_cast<Settings::InputMappingType>(mapping_scope);
        ApplyMappingScope();
    }
    SettingCheckbox("Use Artic Base controller", Settings::values.use_artic_base_controller);

    if (ImGui::Button(polling_auto_map ? "[press the right face button]"
                                       : "Auto-map Xbox controller")) {
        polling_auto_map = !polling_auto_map;
        polling_button = -1;
        polling_analog = -1;
        polling_hotkey = -1;
        polling_menu_chord = -1;
        if (polling_auto_map) {
            StartPolling(false);
        } else {
            StopPolling();
        }
    }
    Tooltip("Press the right-hand face button so SDL can distinguish Xbox and Nintendo label "
            "layouts.");
    ImGui::SameLine();
    if (ImGui::Button("Clear all mappings")) {
        StopPolling();
        polling_button = -1;
        polling_analog = -1;
        polling_hotkey = -1;
        polling_menu_chord = -1;
        polling_auto_map = false;
        for (auto& binding : profile.buttons) {
            binding.clear();
        }
        for (auto& binding : profile.analogs) {
            binding.clear();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Restore Xbox defaults")) {
        StopPolling();
        polling_button = -1;
        polling_analog = -1;
        polling_hotkey = -1;
        polling_menu_chord = -1;
        polling_auto_map = false;
        // A generic all-controller package has no hardware GUID, so it works before WGI reports
        // a pad.
        profile.maptype = Settings::InputMappingType::AllControllers;
        Common::ParamPackage xbox{{"engine", "sdl"}, {"api", "controller"}};
        xbox.Set("guid", "0");
        xbox.Set("port", 0);
        xbox.Set("button", SDL_CONTROLLER_BUTTON_B);
        MapController(xbox);
    }
    if (ImGui::Button("Restore keyboard defaults")) {
        StopPolling();
        polling_button = -1;
        polling_analog = -1;
        polling_hotkey = -1;
        polling_menu_chord = -1;
        polling_auto_map = false;
        KeyboardInput::ApplyDefaultKeys(profile, true);
    }
    Tooltip("Restores the desktop frontend's default keys. Controller bindings are kept.");
    if (ImGui::Button("Add DSU to Xbox mappings")) {
        using DsuButton = InputCommon::CemuhookUDP::Button;
        using DsuAxis = InputCommon::CemuhookUDP::Axis;
        const auto add_button = [&](Settings::NativeButton::Values native, DsuButton remote) {
            std::string local = KeyboardInput::ButtonBinding(profile.buttons[native]);
            const Common::ParamPackage current{local};
            if (current.Get("engine", "") == "cemuhookudp_addon") {
                local = current.Get("local", "");
            }
            if (!local.empty()) {
                KeyboardInput::SetButtonBinding(
                    profile.buttons[native],
                    InputCommon::CemuhookUDP::GenerateAddonButtonParam(local, remote).Serialize());
            }
        };
        add_button(Settings::NativeButton::A, DsuButton::A);
        add_button(Settings::NativeButton::B, DsuButton::B);
        add_button(Settings::NativeButton::X, DsuButton::X);
        add_button(Settings::NativeButton::Y, DsuButton::Y);
        add_button(Settings::NativeButton::Up, DsuButton::Up);
        add_button(Settings::NativeButton::Down, DsuButton::Down);
        add_button(Settings::NativeButton::Left, DsuButton::Left);
        add_button(Settings::NativeButton::Right, DsuButton::Right);
        add_button(Settings::NativeButton::L, DsuButton::L1);
        add_button(Settings::NativeButton::R, DsuButton::R1);
        add_button(Settings::NativeButton::ZL, DsuButton::L2);
        add_button(Settings::NativeButton::ZR, DsuButton::R2);
        add_button(Settings::NativeButton::Start, DsuButton::Options);
        add_button(Settings::NativeButton::Select, DsuButton::Share);
        add_button(Settings::NativeButton::Home, DsuButton::Home);
        const auto add_analog = [&](Settings::NativeAnalog::Values native, DsuAxis x, DsuAxis y) {
            std::string local = KeyboardInput::AnalogBinding(profile.analogs[native]);
            const Common::ParamPackage current{local};
            if (current.Get("engine", "") == "cemuhookudp_addon") {
                local = current.Get("local", "");
            }
            if (!local.empty()) {
                KeyboardInput::SetAnalogBinding(
                    profile.analogs[native],
                    InputCommon::CemuhookUDP::GenerateAddonAnalogParam(local, x, y).Serialize());
            }
        };
        add_analog(Settings::NativeAnalog::CirclePad, DsuAxis::LeftX, DsuAxis::LeftY);
        add_analog(Settings::NativeAnalog::CStick, DsuAxis::RightX, DsuAxis::RightY);
    }
    Tooltip("Add-on mode keeps the current Xbox mappings and merges the selected DSU pad into "
            "them. The stronger analog-stick input wins.");
    if (ImGui::Button("Map standard DSU pad")) {
        StopPolling();
        polling_button = -1;
        polling_analog = -1;
        polling_hotkey = -1;
        polling_menu_chord = -1;
        polling_auto_map = false;
        using DsuButton = InputCommon::CemuhookUDP::Button;
        using DsuAxis = InputCommon::CemuhookUDP::Axis;
        const auto button = [&](Settings::NativeButton::Values native, DsuButton remote) {
            KeyboardInput::SetButtonBinding(
                profile.buttons[native],
                InputCommon::CemuhookUDP::GenerateButtonParam(remote).Serialize());
        };

        button(Settings::NativeButton::A, DsuButton::A);
        button(Settings::NativeButton::B, DsuButton::B);
        button(Settings::NativeButton::X, DsuButton::X);
        button(Settings::NativeButton::Y, DsuButton::Y);
        button(Settings::NativeButton::Up, DsuButton::Up);
        button(Settings::NativeButton::Down, DsuButton::Down);
        button(Settings::NativeButton::Left, DsuButton::Left);
        button(Settings::NativeButton::Right, DsuButton::Right);
        button(Settings::NativeButton::L, DsuButton::L1);
        button(Settings::NativeButton::R, DsuButton::R1);
        button(Settings::NativeButton::ZL, DsuButton::L2);
        button(Settings::NativeButton::ZR, DsuButton::R2);
        button(Settings::NativeButton::Start, DsuButton::Options);
        button(Settings::NativeButton::Select, DsuButton::Share);
        button(Settings::NativeButton::Home, DsuButton::Home);
        KeyboardInput::SetAnalogBinding(
            profile.analogs[Settings::NativeAnalog::CirclePad],
            InputCommon::CemuhookUDP::GenerateAnalogParam(DsuAxis::LeftX, DsuAxis::LeftY)
                .Serialize());
        KeyboardInput::SetAnalogBinding(
            profile.analogs[Settings::NativeAnalog::CStick],
            InputCommon::CemuhookUDP::GenerateAnalogParam(DsuAxis::RightX, DsuAxis::RightY)
                .Serialize());
    }
    Tooltip("Full mode switches the standard 3DS controls to the selected network DSU pad. "
            "Individual rows can still be rebound.");
    ImGui::TextDisabled(
        "Xbox gamepads use SDL/WGI. Remote DSU buttons and sticks may be mapped below. Each "
        "control also takes a keyboard key. During gameplay the left mouse button touches the "
        "bottom screen and Esc opens the menu.");

    GroupHeader("Face buttons");
    DrawButtonRow("A", Settings::NativeButton::A);
    DrawButtonRow("B", Settings::NativeButton::B);
    DrawButtonRow("X", Settings::NativeButton::X);
    DrawButtonRow("Y", Settings::NativeButton::Y);

    GroupHeader("D-Pad");
    DrawButtonRow("Up", Settings::NativeButton::Up);
    DrawButtonRow("Down", Settings::NativeButton::Down);
    DrawButtonRow("Left", Settings::NativeButton::Left);
    DrawButtonRow("Right", Settings::NativeButton::Right);

    GroupHeader("Shoulder buttons");
    DrawButtonRow("L", Settings::NativeButton::L);
    DrawButtonRow("R", Settings::NativeButton::R);
    DrawButtonRow("ZL", Settings::NativeButton::ZL);
    DrawButtonRow("ZR", Settings::NativeButton::ZR);

    GroupHeader("System buttons");
    DrawButtonRow("Start", Settings::NativeButton::Start);
    DrawButtonRow("Select", Settings::NativeButton::Select);
    DrawButtonRow("Debug", Settings::NativeButton::Debug);
    DrawButtonRow("GPIO14", Settings::NativeButton::Gpio14);
    DrawButtonRow("Home", Settings::NativeButton::Home);
    DrawButtonRow("Power", Settings::NativeButton::Power);

    GroupHeader("Analog sticks");
    DrawAnalogRow("Circle Pad", Settings::NativeAnalog::CirclePad);
    DrawAnalogRow("C-Stick", Settings::NativeAnalog::CStick);

    GroupHeader("Touchscreen cursor");
    static constexpr std::array kRightStickFunctions{"C-Stick + touchscreen cursor", "C-Stick only",
                                                     "Touchscreen cursor only"};
    if (SettingEnumCombo("Right stick function", UWP::values.right_stick_function,
                         kRightStickFunctions,
                         "Move the 3DS touchscreen cursor with the Xbox right stick. Press the "
                         "right stick to touch the selected point.")) {
        UWP::NormalizeInputSettings();
        InputCommon::ReloadInputDevices();
    }
    SettingSlider("Cursor sensitivity", UWP::values.touchscreen_cursor_sensitivity, 0.25, 4.0,
                  "%.2fx", "Applies only to Azahar's virtual touchscreen cursor.");
    SettingCheckbox("Hide cursor after 4 seconds", UWP::values.touchscreen_cursor_timeout,
                    "The cursor reappears as soon as it moves or is pressed.");
    ImGui::TextDisabled(
        "DSU touch maps directly to 3DS touch. DSU mouse and touchpad input move this cursor.");

    GroupHeader("DSU remote input");
    ImGui::TextWrapped(
        "Buttons, sticks, touchscreen, and motion can be received from a Cemuhook/DSU server. A "
        "phone or PC server may translate its touch, mouse, keyboard, or sensors into those DSU "
        "controls.");

    std::array<char, 64> address{};
    const auto address_length = std::min(profile.udp_input_address.size(), address.size() - 1);
    std::copy_n(profile.udp_input_address.begin(), address_length, address.begin());
    if (GetVirtualKeyboard().InputText("DSU server IPv4", address.data(), address.size())) {
        const std::string new_address{address.data()};
        if (!new_address.empty() && new_address != profile.udp_input_address) {
            profile.udp_input_address = new_address;
            ReloadDsu();
        }
    }

    int port = profile.udp_input_port;
    if (ImGui::InputInt("DSU port", &port)) {
        profile.udp_input_port = static_cast<u16>(
            std::clamp(port, 1, static_cast<int>(std::numeric_limits<u16>::max())));
        ReloadDsu();
    }
    profile.udp_pad_index = 0;
    ImGui::TextDisabled("DSU controller: Pad 1");

    const int test_result = dsu_async->test_result.load();
    ImGui::BeginDisabled(test_result == 1);
    if (ImGui::Button(test_result == 1 ? "Testing DSU..." : "Test DSU connection")) {
        dsu_async->test_result.store(1);
        const auto state = dsu_async;
        InputCommon::CemuhookUDP::TestCommunication(
            profile.udp_input_address, profile.udp_input_port, profile.udp_pad_index, 24872,
            [state] { state->test_result.store(2); }, [state] { state->test_result.store(3); });
    }
    ImGui::EndDisabled();
    if (test_result == 2) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), "Connected");
    } else if (test_result == 3) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "No DSU pad response");
    }

    Common::ParamPackage touch{profile.touch_device};
    int calibration[4]{touch.Get("min_x", 0), touch.Get("min_y", 0), touch.Get("max_x", 32767),
                       touch.Get("max_y", 32767)};
    const bool calibrated = dsu_async->calibration_ready.exchange(false);
    if (calibrated) {
        calibration[0] = dsu_async->min_x.load();
        calibration[1] = dsu_async->min_y.load();
        calibration[2] = dsu_async->max_x.load();
        calibration[3] = dsu_async->max_y.load();
    }
    if (calibrated || ImGui::InputInt4("Touch bounds (min X/Y, max X/Y)", calibration)) {
        calibration[0] = std::clamp(calibration[0], 0, 65534);
        calibration[1] = std::clamp(calibration[1], 0, 65534);
        calibration[2] = std::clamp(calibration[2], calibration[0] + 1, 65535);
        calibration[3] = std::clamp(calibration[3], calibration[1] + 1, 65535);
        touch.Set("engine", "cemuhookudp");
        touch.Set("min_x", calibration[0]);
        touch.Set("min_y", calibration[1]);
        touch.Set("max_x", calibration[2]);
        touch.Set("max_y", calibration[3]);
        profile.touch_device = touch.Serialize();
    }

    const int calibration_status = dsu_async->calibration_status.load();
    ImGui::BeginDisabled(calibration_status >= 1 && calibration_status < 4);
    if (ImGui::Button("Calibrate DSU touch")) {
        calibration_job.reset();
        dsu_async->calibration_status.store(1);
        const auto state = dsu_async;
        calibration_job = std::make_unique<InputCommon::CemuhookUDP::CalibrationConfigurationJob>(
            profile.udp_input_address, profile.udp_input_port, profile.udp_pad_index, 24872,
            [state](InputCommon::CemuhookUDP::CalibrationConfigurationJob::Status status) {
                state->calibration_status.store(static_cast<int>(status) + 1);
            },
            [state](u16 min_x, u16 min_y, u16 max_x, u16 max_y) {
                state->min_x.store(min_x);
                state->min_y.store(min_y);
                state->max_x.store(max_x);
                state->max_y.store(max_y);
                state->calibration_ready.store(true);
            });
    }
    ImGui::EndDisabled();
    if (calibration_status >= 1 && calibration_status < 4) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel calibration")) {
            calibration_job.reset();
            dsu_async->calibration_status.store(0);
        }
    }
    if (calibration_status == 1) {
        ImGui::SameLine();
        ImGui::TextDisabled("Waiting for DSU packets...");
    } else if (calibration_status == 2) {
        ImGui::SameLine();
        ImGui::TextDisabled("Touch the top-left of the source touch area.");
    } else if (calibration_status == 3) {
        ImGui::SameLine();
        ImGui::TextDisabled("Now touch the bottom-right.");
    } else if (calibration_status == 4) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), "Calibration saved");
    }
}

void ConfigureDialog::DrawButtonRow(const char* label, int button_index) {
    auto& profile = Settings::values.current_input_profile;
    auto& binding = profile.buttons[static_cast<std::size_t>(button_index)];

    ImGui::PushID(button_index);
    ImGui::Text("%-14s", label);
    ImGui::SameLine(ImGui::GetFontSize() * 9.0f);

    const bool awaiting = polling_button == button_index && polling_analog < 0;
    const std::string controller = KeyboardInput::ButtonBinding(binding);
    const std::string binding_text =
        controller.empty() ? std::string{}
                           : InputCommon::ButtonToText(Common::ParamPackage{controller});
    const char* caption = awaiting             ? "[press a button]"
                          : controller.empty() ? "[not set]"
                                               : binding_text.c_str();
    if (ImGui::Button(caption, ImVec2(ImGui::GetFontSize() * 18.0f, 0.0f))) {
        // Capture mode: the event pump writes the next input it sees into this slot.
        polling_button = awaiting ? -1 : button_index;
        polling_analog = -1;
        polling_hotkey = -1;
        polling_menu_chord = -1;
        polling_auto_map = false;
        if (polling_button >= 0) {
            StartPolling(/*analog=*/false);
        } else {
            StopPolling();
        }
    }
    ImGui::SameLine();
    const bool awaiting_key =
        key_capture == KeyCaptureTarget::Button && key_capture_index == button_index;
    const s32 key = KeyboardInput::ButtonKey(binding);
    const std::string key_caption = fmt::format(
        "{}###key",
        awaiting_key ? "[press a key]" : (key == 0 ? "[no key]" : KeyboardInput::KeyName(key)));
    if (ImGui::Button(key_caption.c_str(), ImVec2(ImGui::GetFontSize() * 8.0f, 0.0f))) {
        if (awaiting_key) {
            StopPolling();
        } else {
            StartKeyCapture(KeyCaptureTarget::Button, button_index);
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        KeyboardInput::SetButtonBinding(binding, {});
    }
    ImGui::PopID();
}

void ConfigureDialog::DrawAnalogRow(const char* label, int analog_index) {
    auto& profile = Settings::values.current_input_profile;
    auto& binding = profile.analogs[static_cast<std::size_t>(analog_index)];

    // Offset the id space so an analog index cannot collide with a button index.
    ImGui::PushID(1000 + analog_index);
    ImGui::Text("%-14s", label);
    ImGui::SameLine(ImGui::GetFontSize() * 9.0f);

    const bool awaiting = polling_analog == analog_index;
    const std::string controller = KeyboardInput::AnalogBinding(binding);
    std::string binding_text;
    if (!controller.empty()) {
        const Common::ParamPackage params{controller};
        const std::string axis_x = params.Get("name_x", "Axis " + params.Get("axis_x", ""));
        const std::string axis_y = params.Get("name_y", "Axis " + params.Get("axis_y", ""));
        binding_text = axis_x + " / " + axis_y;
    }
    const char* caption = awaiting             ? "[move a stick]"
                          : controller.empty() ? "[not set]"
                                               : binding_text.c_str();
    if (ImGui::Button(caption, ImVec2(ImGui::GetFontSize() * 18.0f, 0.0f))) {
        polling_analog = awaiting ? -1 : analog_index;
        polling_button = -1;
        polling_hotkey = -1;
        polling_menu_chord = -1;
        polling_auto_map = false;
        if (polling_analog >= 0) {
            StartPolling(/*analog=*/true);
        } else {
            StopPolling();
        }
    }
    ImGui::SameLine();
    const bool awaiting_keys =
        key_capture == KeyCaptureTarget::Analog && key_capture_index == analog_index;
    const KeyboardInput::AnalogKeys keys = KeyboardInput::AnalogKeysOf(binding);
    std::string keys_text = "[no keys]";
    if (awaiting_keys) {
        keys_text = "[press keys]";
    } else if (keys.keys[0] != 0) {
        keys_text =
            fmt::format("{} {} {} {}", KeyboardInput::KeyName(keys.keys[0]),
                        KeyboardInput::KeyName(keys.keys[1]), KeyboardInput::KeyName(keys.keys[2]),
                        KeyboardInput::KeyName(keys.keys[3]));
        if (keys.keys[4] != 0) {
            keys_text += fmt::format(" ({})", KeyboardInput::KeyName(keys.keys[4]));
        }
    }
    if (ImGui::Button(fmt::format("{}###keys", keys_text).c_str(),
                      ImVec2(ImGui::GetFontSize() * 12.0f, 0.0f))) {
        if (awaiting_keys) {
            StopPolling();
        } else {
            StartKeyCapture(KeyCaptureTarget::Analog, analog_index);
        }
    }
    Tooltip("Keys for up, down, left and right, then a modifier key that halves the deflection.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        KeyboardInput::SetAnalogBinding(binding, {});
    }
    if (!controller.empty()) {
        Common::ParamPackage params{controller};
        const std::string engine = params.Get("engine", "");
        if (engine == "sdl" || engine == "cemuhookudp") {
            int deadzone = static_cast<int>(params.Get("deadzone", 0.1f) * 100.0f);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
            if (ImGui::SliderInt("Deadzone", &deadzone, 0, 95, "%d%%")) {
                params.Set("deadzone", deadzone / 100.0f);
                KeyboardInput::SetAnalogBinding(binding, params.Serialize());
            }
        }
    }
    ImGui::PopID();
}

void ConfigureDialog::DrawControllerHotkeysTab() {
    GroupHeader("In-game menu chord");
    ImGui::TextWrapped(
        "Both bindings must be held together. Start by itself remains the emulated 3DS Start "
        "button. The default is View/Select + Menu/Start. On a keyboard, Esc opens the menu.");
    constexpr std::array chord_labels{"First button", "Second button"};
    for (std::size_t i = 0; i < UWP::values.in_game_menu_chord.size(); ++i) {
        ImGui::PushID(static_cast<int>(7000 + i));
        ImGui::TextUnformatted(chord_labels[i]);
        ImGui::SameLine(ImGui::GetFontSize() * 10.0f);
        const std::string& binding = UWP::values.in_game_menu_chord[i];
        const std::string binding_text =
            binding.empty() ? std::string{}
                            : InputCommon::ButtonToText(Common::ParamPackage{binding});
        const bool awaiting = polling_menu_chord == static_cast<int>(i);
        if (ImGui::Button(awaiting          ? "[press a button]"
                          : binding.empty() ? "[not set]"
                                            : binding_text.c_str(),
                          ImVec2(ImGui::GetFontSize() * 18.0f, 0.0f))) {
            polling_menu_chord = awaiting ? -1 : static_cast<int>(i);
            polling_button = -1;
            polling_analog = -1;
            polling_hotkey = -1;
            polling_auto_map = false;
            if (polling_menu_chord >= 0) {
                StartPolling(false);
            } else {
                StopPolling();
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            UWP::values.in_game_menu_chord[i].clear();
        }
        ImGui::PopID();
    }
    const bool incomplete =
        UWP::values.in_game_menu_chord[0].empty() || UWP::values.in_game_menu_chord[1].empty();
    const bool identical =
        !incomplete && UWP::values.in_game_menu_chord[0] == UWP::values.in_game_menu_chord[1];
    if (incomplete || identical) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                           identical ? "Choose two different buttons."
                                     : "Set both buttons to enable the in-game menu chord.");
    }

    GroupHeader("Controller hotkeys");
    ImGui::TextDisabled(
        "Bind native Xbox or remote DSU pad inputs, or keyboard keys, to frontend commands. They "
        "fire any time, including during gameplay.");

    if (!ImGui::BeginTable("##controller_hotkeys", 4,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        return;
    }
    ImGui::TableSetupColumn("Command");
    ImGui::TableSetupColumn("Binding");
    ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::GetFontSize() * 8.0f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 10.0f);
    ImGui::TableHeadersRow();

    for (std::size_t i = 0; i < static_cast<std::size_t>(UWP::ControllerHotkey::Count); ++i) {
        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(5000 + i));

        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(UWP::ControllerHotkeyName(static_cast<UWP::ControllerHotkey>(i)));

        ImGui::TableSetColumnIndex(1);
        const std::string& binding = UWP::values.controller_hotkeys[i];
        const std::string binding_text =
            binding.empty() ? std::string{}
                            : InputCommon::ButtonToText(Common::ParamPackage{binding});
        const bool awaiting = polling_hotkey == static_cast<int>(i);
        ImGui::TextUnformatted(awaiting ? "[press an input]"
                                        : (binding.empty() ? "[not set]" : binding_text.c_str()));

        ImGui::TableSetColumnIndex(2);
        const bool awaiting_key =
            key_capture == KeyCaptureTarget::Hotkey && key_capture_index == static_cast<int>(i);
        const std::string& chord = UWP::values.keyboard_hotkeys[i];
        const std::string key_caption = fmt::format(
            "{}###key", awaiting_key ? "[press keys]" : (chord.empty() ? "[no key]" : chord));
        if (ImGui::SmallButton(key_caption.c_str())) {
            if (awaiting_key) {
                StopPolling();
            } else {
                StartKeyCapture(KeyCaptureTarget::Hotkey, static_cast<int>(i));
            }
        }

        ImGui::TableSetColumnIndex(3);
        if (ImGui::SmallButton("Rebind")) {
            polling_hotkey = static_cast<int>(i);
            polling_button = -1;
            polling_analog = -1;
            polling_menu_chord = -1;
            polling_auto_map = false;
            StartPolling(false);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            UWP::values.controller_hotkeys[i].clear();
        }

        ImGui::PopID();
    }
    ImGui::EndTable();
}

void ConfigureDialog::DrawCameraTab() {
    auto& profile = Settings::values.current_input_profile;
    (void)profile;

    GroupHeader("Camera");
    // Entries are tied to CameraIndex: OuterRight (0), Inner (1), OuterLeft (2).
    struct CameraEntry {
        const char* label;
        Service::CAM::CameraIndex index;
    };
    static constexpr std::array kCameras{
        CameraEntry{"Outer Right", Service::CAM::OuterRightCamera},
        CameraEntry{"Inner", Service::CAM::InnerCamera},
        CameraEntry{"Outer Left", Service::CAM::OuterLeftCamera},
    };
    static constexpr std::array kSourceItems{"Blank", "Still Image", "Live Camera"};

    if (camera_devices_future.valid() &&
        camera_devices_future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        camera_devices = camera_devices_future.get();
        camera_devices_scanned = true;
    }

    for (const auto& camera : kCameras) {
        const auto index = static_cast<std::size_t>(camera.index);
        ImGui::PushID(static_cast<int>(index));
        ImGui::SeparatorText(camera.label);

        const auto& camera_name = Settings::values.camera_name[index];
        int source = camera_name == "image" ? 1 : (camera_name == "uwp" ? 2 : 0);
        if (Combo("Camera source", source, kSourceItems)) {
            Settings::values.camera_name[index] =
                source == 1 ? "image" : (source == 2 ? "uwp" : "blank");
            // camera_config has source-specific meaning, so it is cleared when the source changes.
            Settings::values.camera_config[index].clear();
            if (source == 2 && !camera_devices_scanned && !camera_devices_future.valid()) {
                camera_devices_future =
                    std::async(std::launch::async, UWP::EnumerateLiveCameraDevices);
            }
        }
        int flip = std::clamp(Settings::values.camera_flip[index], 0,
                              static_cast<int>(kCameraFlipItems.size() - 1));
        if (Combo("Flip", flip, kCameraFlipItems)) {
            Settings::values.camera_flip[index] = flip;
        }
        if (source == 1) {
            std::array<char, 512> buffer{};
            const auto& config = Settings::values.camera_config[index];
            const auto length = std::min(config.size(), buffer.size() - 1);
            std::copy_n(config.begin(), length, buffer.begin());
            if (GetVirtualKeyboard().InputText("Image file (PNG)", buffer.data(), buffer.size())) {
                Settings::values.camera_config[index] = buffer.data();
            }
        } else if (source == 2) {
            if (!camera_devices_scanned && !camera_devices_future.valid()) {
                camera_devices_future =
                    std::async(std::launch::async, UWP::EnumerateLiveCameraDevices);
            }

            const auto& configured_id = Settings::values.camera_config[index];
            const char* preview = "Default (first available)";
            if (!configured_id.empty()) {
                const auto selected = std::find_if(
                    camera_devices.begin(), camera_devices.end(),
                    [&configured_id](const auto& device) { return device.id == configured_id; });
                preview = selected != camera_devices.end() ? selected->name.c_str()
                                                           : "Unavailable camera";
            }
            if (ImGui::BeginCombo("Capture device", preview)) {
                const bool default_selected = configured_id.empty();
                if (ImGui::Selectable("Default (first available)", default_selected)) {
                    Settings::values.camera_config[index].clear();
                }
                if (default_selected) {
                    ImGui::SetItemDefaultFocus();
                }
                for (const auto& device : camera_devices) {
                    const bool selected = configured_id == device.id;
                    if (ImGui::Selectable(device.name.c_str(), selected)) {
                        Settings::values.camera_config[index] = device.id;
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            if (camera_devices_future.valid()) {
                ImGui::TextDisabled("Scanning capture devices...");
            } else if (camera_devices_scanned && camera_devices.empty()) {
                ImGui::TextDisabled("No compatible camera is currently connected.");
            }
            if (!camera_devices_future.valid() && ImGui::SmallButton("Refresh cameras")) {
                camera_devices_scanned = false;
                camera_devices_future =
                    std::async(std::launch::async, UWP::EnumerateLiveCameraDevices);
            }
        }
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Live capture requires an Xbox-compatible USB webcam and camera access.");
}

void ConfigureDialog::DrawNetworkTab() {
    auto& settings = Settings::values;

    GroupHeader("Broadcasting");
    SettingCheckbox("Allow Broadcasting", UWP::values.allow_broadcasting,
                    "Allows Azahar to send its main and secondary screens as network broadcasts.");
    ImGui::BeginDisabled(!UWP::values.allow_broadcasting.GetValue());
    SettingCheckbox(
        "Auto-Start Broadcasts", UWP::values.auto_start_broadcasts,
        "Automatically starts configured broadcasts after an application finishes loading.");
    SettingCheckbox("Allow Third-Party Apps to Control Broadcasts",
                    UWP::values.allow_third_party_broadcast_control,
                    "Allows a connected DSU frontend to start and stop broadcasts.");
    ImGui::EndDisabled();

    GroupHeader("Azahar Web Service");
    SettingInputText("API URL", settings.web_api_url,
                     "Base URL of the web service used for the room browser and telemetry.");
    SettingInputText("Token", settings.network_token,
                     "Credential used to identify you to the web service.");
    ImGui::TextDisabled("Leave the token empty to browse public rooms anonymously.");

    GroupHeader("Rich presence");
    // Discord's named pipe cannot be opened from an app container; the presence is shown in-app.
    SettingCheckbox("Show current application in Discord status",
                    UWP::values.enable_discord_presence);
    if (ImGui::Button("Show presence...")) {
        show_discord_presence = true;
    }
    Tooltip("Displays the presence payload in-app, since it cannot be published to Discord.");
}

void ConfigureDialog::DrawUITab() {
    GroupHeader("Game list");
    SettingCheckbox("Hide titles without an icon", UWP::values.game_list_hide_no_icon);
    SettingCheckbox("Put favorites first", UWP::values.game_list_favorite_first);
    SettingCheckbox("Show favorites only", UWP::values.game_list_favorites_only);

    DrawScreenScraperAccount();

    GroupHeader("ScreenScraper library artwork");
    SettingCheckbox("Enable ScreenScraper library data", UWP::values.screenscraper_enabled);
    ImGui::BeginDisabled(!UWP::values.screenscraper_enabled.GetValue());
    SettingCheckbox("Automatically scrape missing games", UWP::values.screenscraper_auto_scrape,
                    "Requests are made one at a time and cached under the active data root.");
    SettingCheckbox("Download metadata", UWP::values.screenscraper_download_metadata);
    ImGui::TextDisabled("Artwork downloads");
    SettingCheckbox("2D box", UWP::values.screenscraper_download_box_2d);
    SettingCheckbox("3D box", UWP::values.screenscraper_download_box_3d);
    SettingCheckbox("2D box back", UWP::values.screenscraper_download_box_2d_back);
    SettingCheckbox("Game logo (HD preferred)", UWP::values.screenscraper_download_game_logo);
    SettingCheckbox("2D cartridge", UWP::values.screenscraper_download_support_2d);
    SettingCheckbox("Recalbox mix v1", UWP::values.screenscraper_download_mix_v1);
    SettingCheckbox("Recalbox mix v2", UWP::values.screenscraper_download_mix_v2);
    SettingCheckbox("Fan art", UWP::values.screenscraper_download_fanart);
    SettingCheckbox("Screenshot", UWP::values.screenscraper_download_screenshot);
    int box_art =
        std::clamp(static_cast<int>(UWP::values.screenscraper_box_art.GetValue()) - 1, 0, 8);
    constexpr std::array box_art_items{
        "2D box",      "3D box",     "Recalbox mix v1",          "Recalbox mix v2",
        "Fan art",     "Screenshot", "Game logo (HD preferred)", "2D box back",
        "2D cartridge"};
    const auto enable_box_art_download = [](ScreenScraperBoxArt selected) {
        switch (selected) {
        case ScreenScraperBoxArt::Box2D:
            UWP::values.screenscraper_download_box_2d.SetValue(true);
            break;
        case ScreenScraperBoxArt::Box3D:
            UWP::values.screenscraper_download_box_3d.SetValue(true);
            break;
        case ScreenScraperBoxArt::MixRecalboxV1:
            UWP::values.screenscraper_download_mix_v1.SetValue(true);
            break;
        case ScreenScraperBoxArt::MixRecalboxV2:
            UWP::values.screenscraper_download_mix_v2.SetValue(true);
            break;
        case ScreenScraperBoxArt::FanArt:
            UWP::values.screenscraper_download_fanart.SetValue(true);
            break;
        case ScreenScraperBoxArt::Screenshot:
            UWP::values.screenscraper_download_screenshot.SetValue(true);
            break;
        case ScreenScraperBoxArt::GameLogo:
            UWP::values.screenscraper_download_game_logo.SetValue(true);
            break;
        case ScreenScraperBoxArt::Box2DBack:
            UWP::values.screenscraper_download_box_2d_back.SetValue(true);
            break;
        case ScreenScraperBoxArt::Support2D:
            UWP::values.screenscraper_download_support_2d.SetValue(true);
            break;
        default:
            break;
        }
    };
    if (ImGui::Combo("Choose Gameslist boxart type", &box_art, box_art_items.data(),
                     static_cast<int>(box_art_items.size()))) {
        const auto selected = static_cast<ScreenScraperBoxArt>(box_art + 1);
        UWP::values.screenscraper_box_art.SetValue(selected);
        enable_box_art_download(selected);
    }
    int large_box_art =
        std::clamp(static_cast<int>(UWP::values.screenscraper_large_box_art.GetValue()) - 1, 0, 8);
    if (ImGui::Combo("Choose large boxart type", &large_box_art, box_art_items.data(),
                     static_cast<int>(box_art_items.size()))) {
        const auto selected = static_cast<ScreenScraperBoxArt>(large_box_art + 1);
        UWP::values.screenscraper_large_box_art.SetValue(selected);
        enable_box_art_download(selected);
    }
    ImGui::TextDisabled("Dynamic video download");
    SettingCheckbox("Video", UWP::values.screenscraper_download_video,
                    "Normalized MP4 previews use more storage and bandwidth.");
    int background = static_cast<int>(UWP::values.screenscraper_background.GetValue());
    constexpr std::array background_items{"Off",   "Fan art",         "Screenshot",
                                          "Video", "Recalbox mix v1", "Recalbox mix v2"};
    if (ImGui::Combo("Dynamic background", &background, background_items.data(),
                     static_cast<int>(background_items.size()))) {
        const auto selected = static_cast<ScreenScraperBackground>(background);
        UWP::values.screenscraper_background.SetValue(selected);
        // A displayed media type must also be downloaded.
        switch (selected) {
        case ScreenScraperBackground::FanArt:
            UWP::values.screenscraper_download_fanart.SetValue(true);
            break;
        case ScreenScraperBackground::Screenshot:
            UWP::values.screenscraper_download_screenshot.SetValue(true);
            break;
        case ScreenScraperBackground::Video:
            UWP::values.screenscraper_download_video.SetValue(true);
            break;
        case ScreenScraperBackground::MixRecalboxV1:
            UWP::values.screenscraper_download_mix_v1.SetValue(true);
            break;
        case ScreenScraperBackground::MixRecalboxV2:
            UWP::values.screenscraper_download_mix_v2.SetValue(true);
            break;
        default:
            break;
        }
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled(
        "Use Tools > Game Library to update artwork and descriptions for the entire library.");

    GroupHeader("Interface");
    SettingSlider("UI scale", UWP::values.ui_scale, 1.0, 4.0, "%.2fx",
                  "Applied on the next launch. Larger values suit a TV viewed from further "
                  "away.");
    SettingCheckbox("Show advanced frametime info", UWP::values.show_advanced_frametime_info);

    // Theme and language are Qt concepts with no equivalent here.
    ImGui::Spacing();
    ImGui::TextDisabled("Theme and language selection are specific to the desktop Qt build.");
}

} // namespace UWP
