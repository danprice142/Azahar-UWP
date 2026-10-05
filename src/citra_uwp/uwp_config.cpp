// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <INIReader.h>
#include <SDL.h>
#include <fmt/format.h>

#include "citra_uwp/storage_paths.h"
#include "citra_uwp/uwp_config.h"
#include "citra_uwp/uwp_settings.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "core/hle/service/service.h"
#include "input_common/udp/client.h"
#include "input_common/udp/udp.h"

namespace UWP {

namespace {

std::unique_ptr<INIReader> ini;

std::string ConfigPath() {
    return FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) + "config.ini";
}

std::string EnsureConfiguredDirectory(std::string selected, std::string_view development_fallback,
                                      std::string_view package_fallback) {
    if (selected.empty()) {
        return {};
    }

    std::array<std::string, 3> candidates{selected, std::string{development_fallback},
                                          std::string{package_fallback}};
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const std::string& candidate = candidates[index];
        if (candidate.empty() || std::any_of(candidates.begin(), candidates.begin() + index,
                                             [&candidate](const std::string& earlier) {
                                                 return Common::ToLower(earlier) ==
                                                        Common::ToLower(candidate);
                                             })) {
            continue;
        }

        std::string error;
        if (Storage::EnsureDirectory(candidate, error)) {
            if (index != 0) {
                LOG_WARNING(Config, "Storage path {} is unavailable; using {}", selected,
                            candidate);
            }
            return candidate;
        }
        LOG_WARNING(Config, "Storage path {} is unavailable: {}", candidate, error);
    }
    return {};
}

constexpr std::string_view PortableUserRoot = "%CITRA_USER_DIR%";

std::string NormalizePath(std::string path, bool trailing_separator) {
    std::replace(path.begin(), path.end(), '/', '\\');
    while (path.size() > 3 && path.back() == '\\') {
        path.pop_back();
    }
    if (trailing_separator && !path.empty() && path.back() != '\\') {
        path.push_back('\\');
    }
    return path;
}

bool EqualPathCharacter(char left, char right) {
    return std::tolower(static_cast<unsigned char>(left)) ==
           std::tolower(static_cast<unsigned char>(right));
}

bool EqualPaths(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), EqualPathCharacter);
}

bool IsPathUnderRoot(std::string_view path, std::string_view root) {
    return path.size() >= root.size() &&
           std::equal(root.begin(), root.end(), path.begin(), EqualPathCharacter);
}

std::string CurrentDataRoot() {
    return NormalizePath(FileUtil::GetUserPath(FileUtil::UserPath::UserDir), true);
}

std::string StoredDataRoot() {
    return NormalizePath(ini->Get("Paths", "data_root", ""), true);
}

std::string ResolvePortablePath(std::string path, std::string_view stored_root) {
    if (path.starts_with(PortableUserRoot)) {
        return CurrentDataRoot() + path.substr(PortableUserRoot.size());
    }

    path = NormalizePath(std::move(path), false);
    const std::string normalized_stored_root = NormalizePath(std::string{stored_root}, true);
    const std::string stored_root_without_separator =
        NormalizePath(std::string{stored_root}, false);
    if (!stored_root_without_separator.empty() && EqualPaths(path, stored_root_without_separator)) {
        return NormalizePath(CurrentDataRoot(), false);
    }
    if (!normalized_stored_root.empty() && IsPathUnderRoot(path, normalized_stored_root)) {
        return CurrentDataRoot() + path.substr(normalized_stored_root.size());
    }
    return path;
}

std::string MakePortablePath(std::string path) {
    path = NormalizePath(std::move(path), false);
    const std::string current_root = CurrentDataRoot();
    if (EqualPaths(path, NormalizePath(current_root, false))) {
        return std::string{PortableUserRoot};
    }
    if (IsPathUnderRoot(path, current_root)) {
        return std::string{PortableUserRoot} + path.substr(current_root.size());
    }
    return path;
}

constexpr int MaxInputProfiles = 32;
constexpr Settings::LayoutOption FallbackSimultaneousLayout = Settings::LayoutOption::SideScreen;
constexpr u64 DefaultVideoBitrate = 2'500'000;
constexpr u64 DefaultAudioBitrate = 64'000;

void ResetVideoDumpingDefaults() {
    auto& settings = Settings::values;
    settings.output_format = "mp4";
    settings.format_options.clear();
    settings.video_encoder = "h264_mf";
    settings.video_encoder_options.clear();
    settings.video_bitrate = DefaultVideoBitrate;
    settings.audio_encoder = "aac";
    settings.audio_encoder_options.clear();
    settings.audio_bitrate = DefaultAudioBitrate;
}

u64 ReadUnsigned(const std::string& group, const std::string& key, u64 fallback) {
    const std::string raw = ini->Get(group, key, std::to_string(fallback));
    u64 value{};
    const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    return error == std::errc{} && end == raw.data() + raw.size() ? value : fallback;
}

bool IsUwpCycleLayout(Settings::LayoutOption layout) {
    switch (layout) {
    case Settings::LayoutOption::Default:
    case Settings::LayoutOption::SingleScreen:
    case Settings::LayoutOption::LargeScreen:
    case Settings::LayoutOption::SideScreen:
    case Settings::LayoutOption::HybridScreen:
    case Settings::LayoutOption::CustomLayout:
        return true;
    case Settings::LayoutOption::SeparateWindows:
        return false;
    }
    return false;
}

std::vector<Settings::LayoutOption> SanitizeCycleLayouts(
    const std::vector<Settings::LayoutOption>& layouts) {
    std::vector<Settings::LayoutOption> result;
    result.reserve(layouts.size());
    for (const auto layout : layouts) {
        if (IsUwpCycleLayout(layout) &&
            std::find(result.begin(), result.end(), layout) == result.end()) {
            result.push_back(layout);
        }
    }
    if (result.empty()) {
        result.push_back(FallbackSimultaneousLayout);
    }
    return result;
}

Settings::LayoutOption SanitizeLayout(Settings::LayoutOption layout) {
    return IsUwpCycleLayout(layout) ? layout : FallbackSimultaneousLayout;
}

template <typename Type, bool ranged, typename Sanitize>
void SanitizeSwitchableSlots(Settings::SwitchableSetting<Type, ranged>& setting,
                             const Sanitize& sanitize) {
    const bool was_using_global = setting.UsingGlobal();
    const auto sanitize_selected_slot = [&] {
        const Type sanitized = sanitize(setting.GetValue());
        if (setting.GetValue() != sanitized) {
            setting.SetValue(sanitized);
        }
    };

    // Global config.ini is saved even while an override is active, so always repair the global
    // slot.
    setting.SetGlobal(true);
    sanitize_selected_slot();
    if (!was_using_global) {
        setting.SetGlobal(false);
        sanitize_selected_slot();
    }
    setting.SetGlobal(was_using_global);
}

std::string SerializeCycleLayouts(const std::vector<Settings::LayoutOption>& layouts) {
    std::ostringstream out;
    const auto sanitized = SanitizeCycleLayouts(layouts);
    for (std::size_t i = 0; i < sanitized.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << static_cast<std::underlying_type_t<Settings::LayoutOption>>(sanitized[i]);
    }
    return out.str();
}

std::vector<Settings::LayoutOption> ParseCycleLayouts(
    const std::string& text, const std::vector<Settings::LayoutOption>& fallback) {
    std::vector<Settings::LayoutOption> parsed;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find(',', begin);
        std::string_view item{text.data() + begin,
                              (end == std::string::npos ? text.size() : end) - begin};
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) {
            item.remove_prefix(1);
        }
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) {
            item.remove_suffix(1);
        }
        int raw{};
        const auto [parse_end, error] =
            std::from_chars(item.data(), item.data() + item.size(), raw);
        if (error == std::errc{} && parse_end == item.data() + item.size()) {
            const auto layout = static_cast<Settings::LayoutOption>(raw);
            if (IsUwpCycleLayout(layout)) {
                parsed.push_back(layout);
            }
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return SanitizeCycleLayouts(parsed.empty() ? fallback : parsed);
}

std::string DefaultDsuMotionDevice() {
    return Common::ParamPackage{{"engine", "cemuhookudp"}}.Serialize();
}

std::string NormalizeDsuTouchDevice(const std::string& existing = {}) {
    Common::ParamPackage params{existing};
    if (params.Get("engine", "") != "cemuhookudp") {
        params.Clear();
    }
    params.Set("engine", "cemuhookudp");
    int min_x = std::clamp(params.Get("min_x", 0), 0, 65534);
    int min_y = std::clamp(params.Get("min_y", 0), 0, 65534);
    int max_x = std::clamp(params.Get("max_x", 32767), min_x + 1, 65535);
    int max_y = std::clamp(params.Get("max_y", 32767), min_y + 1, 65535);
    if (min_x == 100 && min_y == 50 && max_x == 1800 && max_y == 850) {
        min_x = 0;
        min_y = 0;
        max_x = 32767;
        max_y = 32767;
    }
    params.Set("min_x", min_x);
    params.Set("min_y", min_y);
    params.Set("max_x", max_x);
    params.Set("max_y", max_y);
    return params.Serialize();
}

bool IsDsuButtonBinding(const Common::ParamPackage& params) {
    const std::string engine = params.Get("engine", "");
    if (engine != "cemuhookudp" && engine != "cemuhookudp_addon") {
        return false;
    }
    if (engine == "cemuhookudp_addon") {
        const Common::ParamPackage local{params.Get("local", "")};
        if (local.Get("engine", "") != "sdl" || local.Get("api", "") != "controller") {
            return false;
        }
    }
    if (params.Has("button")) {
        const int button = params.Get("button", -1);
        return button >= 0 && button < static_cast<int>(InputCommon::CemuhookUDP::Button::Count);
    }
    if (params.Has("axis")) {
        const int axis = params.Get("axis", -1);
        const std::string direction = params.Get("direction", "+");
        return axis >= 0 && axis < static_cast<int>(InputCommon::CemuhookUDP::Axis::Count) &&
               (direction == "+" || direction == "-");
    }
    return false;
}

bool IsDsuAnalogBinding(const Common::ParamPackage& params) {
    const std::string engine = params.Get("engine", "");
    if ((engine != "cemuhookudp" && engine != "cemuhookudp_addon") || !params.Has("axis_x") ||
        !params.Has("axis_y")) {
        return false;
    }
    if (engine == "cemuhookudp_addon") {
        const Common::ParamPackage local{params.Get("local", "")};
        if (local.Get("engine", "") != "sdl" || local.Get("api", "") != "controller") {
            return false;
        }
    }
    const int axis_x = params.Get("axis_x", -1);
    const int axis_y = params.Get("axis_y", -1);
    const int axis_count = static_cast<int>(InputCommon::CemuhookUDP::Axis::Count);
    return axis_x >= 0 && axis_x < axis_count && axis_y >= 0 && axis_y < axis_count;
}

bool IsSupportedButtonBinding(const Common::ParamPackage& params) {
    return (params.Get("engine", "") == "sdl" && params.Get("api", "") == "controller") ||
           IsDsuButtonBinding(params);
}

bool IsSupportedAnalogBinding(const Common::ParamPackage& params) {
    return (params.Get("engine", "") == "sdl" && params.Get("api", "") == "controller") ||
           IsDsuAnalogBinding(params);
}

void NormalizeInputSettings(Settings::InputProfile& profile) {
    if (profile.name.empty()) {
        profile.name = "Default";
    }
    if (profile.udp_input_address.empty()) {
        profile.udp_input_address = InputCommon::CemuhookUDP::DEFAULT_ADDR;
    }
    if (profile.udp_input_port == 0) {
        profile.udp_input_port = InputCommon::CemuhookUDP::DEFAULT_PORT;
    }
    // UWP exposes one local player through DSU protocol slot zero.
    profile.udp_pad_index = 0;
    profile.motion_device = DefaultDsuMotionDevice();
    profile.touch_device = NormalizeDsuTouchDevice(profile.touch_device);
    const bool touchscreen_cursor =
        values.right_stick_function.GetValue() != RightStickFunction::CStick;
    profile.controller_touch_device = touchscreen_cursor ? "engine:emu_window" : "";
    profile.use_touchpad = touchscreen_cursor;
    profile.use_touch_from_button = false;
    profile.touch_from_button_map_index = 0;
    Settings::values.c_stick_touchscreen_only.SetValue(values.right_stick_function.GetValue() ==
                                                       RightStickFunction::Touchscreen);

    // Only SDL game controllers and DSU are supported; drop desktop input engines from copied
    // configs.
    const auto keep_supported = [](std::string& binding, bool button) {
        if (binding.empty()) {
            return;
        }
        const Common::ParamPackage params{binding};
        if (button ? !IsSupportedButtonBinding(params) : !IsSupportedAnalogBinding(params)) {
            binding.clear();
        }
    };
    for (auto& binding : profile.buttons) {
        keep_supported(binding, true);
    }
    for (auto& binding : profile.analogs) {
        keep_supported(binding, false);
    }
}

void EnsureProfileList() {
    auto& settings = Settings::values;
    NormalizeInputSettings(settings.current_input_profile);
    if (settings.input_profiles.empty()) {
        settings.input_profiles.push_back(settings.current_input_profile);
        settings.current_input_profile_index = 0;
    }
    settings.current_input_profile_index =
        std::clamp(settings.current_input_profile_index, 0,
                   static_cast<int>(settings.input_profiles.size()) - 1);
}

// Reading

void Read(const std::string& group, Settings::Setting<std::string>& setting) {
    setting.SetValue(ini->Get(group, setting.GetLabel(), setting.GetValue()));
}

void Read(const std::string& group, Settings::Setting<bool>& setting) {
    setting.SetValue(ini->GetBoolean(group, setting.GetLabel(), setting.GetValue()));
}

template <typename Type, bool ranged>
void Read(const std::string& group, Settings::Setting<Type, ranged>& setting) {
    if constexpr (std::is_floating_point_v<Type>) {
        setting.SetValue(static_cast<Type>(
            ini->GetReal(group, setting.GetLabel(), static_cast<double>(setting.GetValue()))));
    } else if constexpr (std::is_enum_v<Type>) {
        setting.SetValue(static_cast<Type>(
            ini->GetInteger(group, setting.GetLabel(), static_cast<long>(setting.GetValue()))));
    } else {
        const std::string fallback = std::to_string(setting.GetValue());
        const std::string raw = ini->Get(group, setting.GetLabel(), fallback);
        Type value{};
        const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (error == std::errc{} && end == raw.data() + raw.size()) {
            setting.SetValue(value);
        }
    }
}

// SwitchableSetting global values; SetValue routes to the global slot before any game runs.
template <typename Type, bool ranged>
void Read(const std::string& group, Settings::SwitchableSetting<Type, ranged>& setting) {
    if constexpr (std::is_same_v<Type, std::string>) {
        setting.SetValue(ini->Get(group, setting.GetLabel(), setting.GetValue()));
    } else if constexpr (std::is_same_v<Type, bool>) {
        setting.SetValue(ini->GetBoolean(group, setting.GetLabel(), setting.GetValue()));
    } else if constexpr (std::is_floating_point_v<Type>) {
        setting.SetValue(static_cast<Type>(
            ini->GetReal(group, setting.GetLabel(), static_cast<double>(setting.GetValue()))));
    } else if constexpr (std::is_enum_v<Type>) {
        setting.SetValue(static_cast<Type>(
            ini->GetInteger(group, setting.GetLabel(), static_cast<long>(setting.GetValue()))));
    } else {
        const std::string fallback = std::to_string(setting.GetValue());
        const std::string raw = ini->Get(group, setting.GetLabel(), fallback);
        Type value{};
        const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (error == std::errc{} && end == raw.data() + raw.size()) {
            setting.SetValue(value);
        }
    }
}

void Read(const std::string& group,
          Settings::SwitchableSetting<std::vector<Settings::LayoutOption>>& setting) {
    const auto global = setting.GetValue(/*need_global=*/true);
    setting.SetValue(ParseCycleLayouts(
        ini->Get(group, setting.GetLabel(), SerializeCycleLayouts(global)), global));
}

// Writing

void WriteValue(std::ostringstream& out, const std::string& label,
                const std::vector<Settings::LayoutOption>& value) {
    out << label << " = " << SerializeCycleLayouts(value) << '\n';
}

template <typename Type>
void WriteValue(std::ostringstream& out, const std::string& label, const Type& value) {
    if constexpr (std::is_same_v<Type, std::string>) {
        out << label << " = " << value << '\n';
    } else if constexpr (std::is_same_v<Type, bool>) {
        out << label << " = " << (value ? "true" : "false") << '\n';
    } else if constexpr (std::is_floating_point_v<Type>) {
        out << label << " = " << value << '\n';
    } else if constexpr (std::is_enum_v<Type>) {
        out << label << " = " << static_cast<std::underlying_type_t<Type>>(value) << '\n';
    } else {
        out << label << " = " << +value << '\n';
    }
}

template <typename Type, bool ranged>
void Write(std::ostringstream& out, const Settings::Setting<Type, ranged>& setting) {
    WriteValue(out, setting.GetLabel(), setting.GetValue());
}

// Global config.ini must serialize the global slot even while a title override is active;
// without this overload GetValue() would return the custom slot.
template <typename Type, bool ranged>
void Write(std::ostringstream& out, const Settings::SwitchableSetting<Type, ranged>& setting) {
    WriteValue(out, setting.GetLabel(), setting.GetValue(/*need_global=*/true));
}

void WriteRaw(std::ostringstream& out, const std::string& key, const std::string& value) {
    out << key << " = " << value << '\n';
}

template <typename Type, bool ranged>
void ReadPerGameSetting(const INIReader& reader, const std::string& group,
                        Settings::SwitchableSetting<Type, ranged>& setting) {
    const std::string use_global_key = setting.GetLabel() + "/use_global";
    const bool use_global = reader.GetBoolean(group, use_global_key, true);
    const Type global_value = setting.GetValue(/*need_global=*/true);
    setting.SetGlobal(use_global);
    if (use_global) {
        return;
    }

    if constexpr (std::is_same_v<Type, std::string>) {
        setting.SetValue(reader.Get(group, setting.GetLabel(), global_value));
    } else if constexpr (std::is_same_v<Type, bool>) {
        setting.SetValue(reader.GetBoolean(group, setting.GetLabel(), global_value));
    } else if constexpr (std::is_floating_point_v<Type>) {
        setting.SetValue(static_cast<Type>(
            reader.GetReal(group, setting.GetLabel(), static_cast<double>(global_value))));
    } else if constexpr (std::is_enum_v<Type>) {
        setting.SetValue(static_cast<Type>(
            reader.GetInteger(group, setting.GetLabel(), static_cast<long>(global_value))));
    } else {
        const std::string fallback = std::to_string(global_value);
        const std::string raw = reader.Get(group, setting.GetLabel(), fallback);
        Type value{};
        const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        setting.SetValue(error == std::errc{} && end == raw.data() + raw.size() ? value
                                                                                : global_value);
    }
}

void ReadPerGameSetting(const INIReader& reader, const std::string& group,
                        Settings::SwitchableSetting<std::vector<Settings::LayoutOption>>& setting) {
    const std::string use_global_key = setting.GetLabel() + "/use_global";
    const bool use_global = reader.GetBoolean(group, use_global_key, true);
    const auto global_value = setting.GetValue(/*need_global=*/true);
    setting.SetGlobal(use_global);
    if (!use_global) {
        setting.SetValue(ParseCycleLayouts(
            reader.Get(group, setting.GetLabel(), SerializeCycleLayouts(global_value)),
            global_value));
    }
}

template <typename Type, bool ranged>
void WritePerGameSetting(std::ostringstream& out,
                         const Settings::SwitchableSetting<Type, ranged>& setting) {
    WriteValue(out, setting.GetLabel() + "/use_global", setting.UsingGlobal());
    if (!setting.UsingGlobal()) {
        WriteValue(out, setting.GetLabel(), setting.GetValue());
    }
}

template <typename... SettingsTypes>
void ReadPerGameSection(const INIReader& reader, const char* group, SettingsTypes&... settings) {
    (ReadPerGameSetting(reader, group, settings), ...);
}

template <typename... SettingsTypes>
void WritePerGameSection(std::ostringstream& out, const char* group,
                         const SettingsTypes&... settings) {
    out << '[' << group << "]\n";
    (WritePerGameSetting(out, settings), ...);
    out << '\n';
}

void ReadPerGameValues(const INIReader& reader) {
    auto& settings = Settings::values;
    std::string stored_data_root = reader.Get("Paths", "data_root", "");
    if (stored_data_root.empty() && ini) {
        stored_data_root = StoredDataRoot();
    } else {
        stored_data_root = NormalizePath(std::move(stored_data_root), true);
    }

    // The UWP subset of the desktop per-game configuration.
    ReadPerGameSection(reader, "Core", settings.cpu_clock_percentage);
    ReadPerGameSection(reader, "System", settings.is_new_3ds, settings.lle_applets,
                       settings.enable_required_online_lle_modules, settings.region_value);
    ReadPerGameSection(
        reader, "Renderer", settings.use_hw_shader, settings.shaders_accurate_mul,
        settings.async_shader_compilation, settings.async_presentation,
        settings.use_disk_shader_cache, settings.use_vsync, settings.use_skip_duplicate_frames,
        settings.use_display_refresh_rate_detection, settings.resolution_factor,
        settings.use_integer_scaling, settings.frame_limit, settings.turbo_limit,
        settings.texture_filter, settings.texture_sampling, settings.delay_game_render_thread_us,
        settings.simulate_3ds_gpu_timings, settings.render_3d, settings.factor_3d,
        settings.swap_eyes_3d, settings.disable_right_eye_render, settings.filter_mode,
        settings.bg_red, settings.bg_green, settings.bg_blue);
    ReadPerGameSection(reader, "Layout", settings.layout_option, settings.swap_screen,
                       settings.upright_screen, settings.large_screen_proportion,
                       settings.screen_gap, settings.small_screen_position,
                       settings.layouts_to_cycle);
    ReadPerGameSection(reader, "Audio", settings.audio_emulation, settings.enable_audio_stretching,
                       settings.enable_realtime_audio, settings.volume,
                       settings.simulate_headphones_plugged);
    ReadPerGameSection(reader, "Utility", settings.dump_textures, settings.custom_textures,
                       settings.preload_textures, settings.async_custom_loading);
    ReadPerGameSetting(reader, "UWP", values.screenshot_path);
    if (!values.screenshot_path.UsingGlobal()) {
        values.screenshot_path.SetValue(
            ResolvePortablePath(values.screenshot_path.GetValue(), stored_data_root));
    }
}

void WritePerGameValues(std::ostringstream& out) {
    const auto& settings = Settings::values;

    WritePerGameSection(out, "Core", settings.cpu_clock_percentage);
    WritePerGameSection(out, "System", settings.is_new_3ds, settings.lle_applets,
                        settings.enable_required_online_lle_modules, settings.region_value);
    WritePerGameSection(
        out, "Renderer", settings.use_hw_shader, settings.shaders_accurate_mul,
        settings.async_shader_compilation, settings.async_presentation,
        settings.use_disk_shader_cache, settings.use_vsync, settings.use_skip_duplicate_frames,
        settings.use_display_refresh_rate_detection, settings.resolution_factor,
        settings.use_integer_scaling, settings.frame_limit, settings.turbo_limit,
        settings.texture_filter, settings.texture_sampling, settings.delay_game_render_thread_us,
        settings.simulate_3ds_gpu_timings, settings.render_3d, settings.factor_3d,
        settings.swap_eyes_3d, settings.disable_right_eye_render, settings.filter_mode,
        settings.bg_red, settings.bg_green, settings.bg_blue);
    WritePerGameSection(out, "Layout", settings.layout_option, settings.swap_screen,
                        settings.upright_screen, settings.large_screen_proportion,
                        settings.screen_gap, settings.small_screen_position,
                        settings.layouts_to_cycle);
    WritePerGameSection(out, "Audio", settings.audio_emulation, settings.enable_audio_stretching,
                        settings.enable_realtime_audio, settings.volume,
                        settings.simulate_headphones_plugged);
    WritePerGameSection(out, "Utility", settings.dump_textures, settings.custom_textures,
                        settings.preload_textures, settings.async_custom_loading);
    out << "[UWP]\n";
    WriteValue(out, values.screenshot_path.GetLabel() + "/use_global",
               values.screenshot_path.UsingGlobal());
    if (!values.screenshot_path.UsingGlobal()) {
        WriteValue(out, values.screenshot_path.GetLabel(),
                   MakePortablePath(values.screenshot_path.GetValue()));
    }
    out << "\n[Paths]\n";
    WriteRaw(out, "data_root", CurrentDataRoot());
    out << '\n';
}

void ReadProfile(const std::string& group, Settings::InputProfile& profile) {
    profile.name = ini->Get(group, "name", profile.name);
    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        profile.buttons[i] =
            ini->Get(group, Settings::NativeButton::mapping[i], profile.buttons[i]);
    }
    for (int i = 0; i < Settings::NativeAnalog::NumAnalogs; ++i) {
        profile.analogs[i] =
            ini->Get(group, Settings::NativeAnalog::mapping[i], profile.analogs[i]);
    }
    profile.maptype = static_cast<Settings::InputMappingType>(std::clamp<long>(
        ini->GetInteger(group, "mapping_type", static_cast<long>(profile.maptype)), 0, 2));
    profile.touch_device = ini->Get(group, "touch_device", profile.touch_device);
    profile.udp_input_address = ini->Get(group, "udp_input_address", profile.udp_input_address);
    profile.udp_input_port = static_cast<u16>(
        std::clamp<long>(ini->GetInteger(group, "udp_input_port", profile.udp_input_port), 1,
                         std::numeric_limits<u16>::max()));
    profile.udp_pad_index = static_cast<u8>(
        std::clamp<long>(ini->GetInteger(group, "udp_pad_index", profile.udp_pad_index), 0, 3));
    NormalizeInputSettings(profile);
}

void WriteProfile(std::ostringstream& out, std::size_t index,
                  const Settings::InputProfile& profile) {
    out << "\n[Controls.Profile" << index << "]\n";
    WriteRaw(out, "name", profile.name);
    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        WriteRaw(out, Settings::NativeButton::mapping[i], profile.buttons[i]);
    }
    for (int i = 0; i < Settings::NativeAnalog::NumAnalogs; ++i) {
        WriteRaw(out, Settings::NativeAnalog::mapping[i], profile.analogs[i]);
    }
    WriteRaw(out, "mapping_type", std::to_string(static_cast<int>(profile.maptype)));
    WriteRaw(out, "motion_device", profile.motion_device);
    WriteRaw(out, "touch_device", profile.touch_device);
    WriteRaw(out, "udp_input_address", profile.udp_input_address);
    WriteRaw(out, "udp_input_port", std::to_string(profile.udp_input_port));
    WriteRaw(out, "udp_pad_index", std::to_string(profile.udp_pad_index));
}

} // namespace

void NormalizeInputSettings() {
    NormalizeInputSettings(Settings::values.current_input_profile);
}

void NormalizeDisplaySettings() {
    auto& settings = Settings::values;
    SanitizeSwitchableSlots(settings.layout_option, SanitizeLayout);
    SanitizeSwitchableSlots(settings.layouts_to_cycle,
                            [](const auto& layouts) { return SanitizeCycleLayouts(layouts); });
}

void RestoreGlobalConfig() {
    auto& settings = Settings::values;
    const auto use_global = [](auto& setting) { setting.SetGlobal(true); };

    // Exhaustive on purpose; Settings::RestoreGlobalState() can lag newer switchable settings.
    use_global(settings.enable_gamemode);
    use_global(settings.cpu_clock_percentage);
    use_global(settings.is_new_3ds);
    use_global(settings.lle_applets);
    use_global(settings.deterministic_async_operations);
    use_global(settings.enable_required_online_lle_modules);
    use_global(settings.region_value);

    use_global(settings.graphics_api);
    use_global(settings.physical_device);
    use_global(settings.spirv_shader_gen);
    use_global(settings.disable_spirv_optimizer);
    use_global(settings.async_shader_compilation);
    use_global(settings.async_presentation);
    use_global(settings.use_hw_shader);
    use_global(settings.use_disk_shader_cache);
    use_global(settings.use_skip_duplicate_frames);
    use_global(settings.shaders_accurate_mul);
    use_global(settings.use_vsync);
    use_global(settings.use_display_refresh_rate_detection);
    use_global(settings.resolution_factor);
    use_global(settings.use_integer_scaling);
    use_global(settings.frame_limit);
    use_global(settings.turbo_limit);
    use_global(settings.texture_filter);
    use_global(settings.texture_sampling);
    use_global(settings.delay_game_render_thread_us);
    use_global(settings.simulate_3ds_gpu_timings);

    use_global(settings.layout_option);
    use_global(settings.swap_screen);
    use_global(settings.upright_screen);
    use_global(settings.secondary_display_layout);
    use_global(settings.layouts_to_cycle);
    use_global(settings.large_screen_proportion);
    use_global(settings.screen_gap);
    use_global(settings.small_screen_position);
    use_global(settings.aspect_ratio);
    use_global(settings.screen_top_stretch);
    use_global(settings.screen_bottom_stretch);
    use_global(settings.portrait_layout_option);
    use_global(settings.bg_red);
    use_global(settings.bg_green);
    use_global(settings.bg_blue);
    use_global(settings.render_3d);
    use_global(settings.factor_3d);
    use_global(settings.swap_eyes_3d);
    use_global(settings.render_3d_which_display);
    use_global(settings.mono_render_option);
    use_global(settings.filter_mode);
    use_global(settings.pp_shader_name);
    use_global(settings.anaglyph_shader_name);
    use_global(settings.dump_textures);
    use_global(settings.custom_textures);
    use_global(settings.preload_textures);
    use_global(settings.async_custom_loading);
    use_global(settings.disable_right_eye_render);

    use_global(settings.audio_emulation);
    use_global(settings.enable_audio_stretching);
    use_global(settings.enable_realtime_audio);
    use_global(settings.volume);
    use_global(settings.simulate_headphones_plugged);
    use_global(values.screenshot_path);

    NormalizeDisplaySettings();
}

void LoadConfig() {
    RestoreGlobalConfig();
    NormalizeInputSettings();
    ResetVideoDumpingDefaults();
    std::string buffer;
    FileUtil::ReadFileToString(true, ConfigPath(), buffer);
    if (buffer.empty()) {
        EnsureProfileList();
        LOG_INFO(Config, "No config.ini yet; using defaults");
        return;
    }
    ini = std::make_unique<INIReader>(buffer.c_str(), buffer.size());
    if (ini->ParseError() != 0) {
        LOG_ERROR(Config, "config.ini is unreadable; using defaults");
        ini.reset();
        EnsureProfileList();
        return;
    }
    const std::string stored_data_root = StoredDataRoot();
    auto& settings = Settings::values;
    const int profile_count = static_cast<int>(
        std::clamp<long>(ini->GetInteger("Controls", "profile_count", 0), 0, MaxInputProfiles));
    settings.input_profiles.clear();
    if (profile_count > 0) {
        for (int i = 0; i < profile_count; ++i) {
            Settings::InputProfile profile{};
            profile.name = fmt::format("Profile {}", i + 1);
            NormalizeInputSettings(profile);
            ReadProfile(fmt::format("Controls.Profile{}", i), profile);
            settings.input_profiles.emplace_back(std::move(profile));
        }
        settings.current_input_profile_index = static_cast<int>(std::clamp<long>(
            ini->GetInteger("Controls", "current_profile", 0), 0, profile_count - 1));
        settings.current_input_profile =
            settings.input_profiles[settings.current_input_profile_index];
    } else {
        EnsureProfileList();
    }
    Read("Controls", Settings::values.use_artic_base_controller);

    // Core
    Read("Core", Settings::values.enable_gamemode);
    Read("Core", Settings::values.use_cpu_jit);
    Read("Core", Settings::values.cpu_clock_percentage);

    // Renderer
    Read("Renderer", Settings::values.graphics_api);
    Read("Renderer", Settings::values.physical_device);
    Read("Renderer", Settings::values.spirv_shader_gen);
    Read("Renderer", Settings::values.disable_spirv_optimizer);
    Read("Renderer", Settings::values.use_hw_shader);
    Read("Renderer", Settings::values.use_shader_jit);
    Read("Renderer", Settings::values.shaders_accurate_mul);
    Read("Renderer", Settings::values.async_shader_compilation);
    Read("Renderer", Settings::values.async_presentation);
    Read("Renderer", Settings::values.simulate_3ds_gpu_timings);
    Read("Renderer", Settings::values.resolution_factor);
    Read("Renderer", Settings::values.use_disk_shader_cache);
    Read("Renderer", Settings::values.use_vsync);
    Read("Renderer", Settings::values.use_skip_duplicate_frames);
    Read("Renderer", Settings::values.use_display_refresh_rate_detection);
    Read("Renderer", Settings::values.texture_filter);
    Read("Renderer", Settings::values.texture_sampling);
    Read("Renderer", Settings::values.delay_game_render_thread_us);
    Read("Renderer", Settings::values.frame_limit);
    Read("Renderer", Settings::values.turbo_limit);
    Read("Renderer", Settings::values.render_3d);
    Read("Renderer", Settings::values.factor_3d);
    Read("Renderer", Settings::values.mono_render_option);
    Read("Renderer", Settings::values.swap_eyes_3d);
    Read("Renderer", Settings::values.disable_right_eye_render);
    Read("Renderer", Settings::values.filter_mode);
    Read("Renderer", Settings::values.use_integer_scaling);
    Read("Renderer", Settings::values.bg_red);
    Read("Renderer", Settings::values.bg_green);
    Read("Renderer", Settings::values.bg_blue);
    Settings::values.pp_shader_name.SetValue(
        ini->Get("Renderer", "pp_shader_name", Settings::values.pp_shader_name.GetValue()));

    // Layout
    Read("Layout", Settings::values.layout_option);
    Read("Layout", Settings::values.swap_screen);
    Read("Layout", Settings::values.upright_screen);
    Read("Layout", Settings::values.screen_gap);
    Read("Layout", Settings::values.large_screen_proportion);
    Read("Layout", Settings::values.small_screen_position);
    Read("Layout", Settings::values.layouts_to_cycle);
    Read("Layout", Settings::values.aspect_ratio);
    Read("Layout", Settings::values.screen_top_stretch);
    Read("Layout", Settings::values.screen_top_leftright_padding);
    Read("Layout", Settings::values.screen_top_topbottom_padding);
    Read("Layout", Settings::values.screen_bottom_stretch);
    Read("Layout", Settings::values.screen_bottom_leftright_padding);
    Read("Layout", Settings::values.screen_bottom_topbottom_padding);
    Read("Layout", Settings::values.custom_top_x);
    Read("Layout", Settings::values.custom_top_y);
    Read("Layout", Settings::values.custom_top_width);
    Read("Layout", Settings::values.custom_top_height);
    Read("Layout", Settings::values.custom_bottom_x);
    Read("Layout", Settings::values.custom_bottom_y);
    Read("Layout", Settings::values.custom_bottom_width);
    Read("Layout", Settings::values.custom_bottom_height);
    Read("Layout", Settings::values.custom_second_layer_opacity);

    // Audio
    Read("Audio", Settings::values.audio_emulation);
    Read("Audio", Settings::values.enable_audio_stretching);
    Read("Audio", Settings::values.enable_realtime_audio);
    Read("Audio", Settings::values.volume);
    Read("Audio", Settings::values.output_type);
    Read("Audio", Settings::values.output_device);
    Read("Audio", Settings::values.input_type);
    Read("Audio", Settings::values.input_device);
    Read("Audio", Settings::values.simulate_headphones_plugged);

    // Video dumping. Raw values, matching the desktop frontend's [VideoDumping] group.
    Settings::values.output_format =
        ini->Get("VideoDumping", "output_format", Settings::values.output_format);
    Settings::values.format_options =
        ini->Get("VideoDumping", "format_options", Settings::values.format_options);
    Settings::values.video_encoder =
        ini->Get("VideoDumping", "video_encoder", Settings::values.video_encoder);
    Settings::values.video_encoder_options =
        ini->Get("VideoDumping", "video_encoder_options", Settings::values.video_encoder_options);
    Settings::values.video_bitrate =
        ReadUnsigned("VideoDumping", "video_bitrate", Settings::values.video_bitrate);
    Settings::values.audio_encoder =
        ini->Get("VideoDumping", "audio_encoder", Settings::values.audio_encoder);
    Settings::values.audio_encoder_options =
        ini->Get("VideoDumping", "audio_encoder_options", Settings::values.audio_encoder_options);
    Settings::values.audio_bitrate =
        ReadUnsigned("VideoDumping", "audio_bitrate", Settings::values.audio_bitrate);
    // Utility
    Read("Utility", Settings::values.dump_textures);
    Read("Utility", Settings::values.custom_textures);
    Read("Utility", Settings::values.preload_textures);
    Read("Utility", Settings::values.async_custom_loading);

    // Storage
    Read("Storage", Settings::values.use_virtual_sd);
    Read("Storage", Settings::values.use_custom_storage);
    Read("Storage", Settings::values.compress_cia_installs);
    Read("Storage", Settings::values.async_fs_operations);

    // System
    Read("System", Settings::values.is_new_3ds);
    Read("System", Settings::values.lle_applets);
    Read("System", Settings::values.enable_required_online_lle_modules);
    Read("System", Settings::values.region_value);
    Read("System", Settings::values.init_clock);
    Read("System", Settings::values.init_time);
    Read("System", Settings::values.init_time_offset);
    Read("System", Settings::values.init_ticks_type);
    Read("System", Settings::values.init_ticks_override);
    Read("System", Settings::values.plugin_loader_enabled);
    Read("System", Settings::values.allow_plugin_loader);
    Read("System", Settings::values.steps_per_hour);
    Read("System", Settings::values.apply_region_free_patch);

    // Camera
    for (int i = 0; i < 3; ++i) {
        Settings::values.camera_name[i] =
            ini->Get("Camera", fmt::format("camera_{}_name", i), Settings::values.camera_name[i]);
        Settings::values.camera_config[i] = ini->Get("Camera", fmt::format("camera_{}_config", i),
                                                     Settings::values.camera_config[i]);
        Settings::values.camera_flip[i] = static_cast<int>(ini->GetInteger(
            "Camera", fmt::format("camera_{}_flip", i), Settings::values.camera_flip[i]));
    }

    // Miscellaneous. The default matches the assignment in main(), not the Setting's default.
    Settings::values.log_filter.SetValue(ini->Get("Miscellaneous", "log_filter", "*:Info"));
    Read("Miscellaneous", Settings::values.log_regex_filter);

    // Debugging
    Read("Debugging", Settings::values.instant_debug_log);
    Read("Debugging", Settings::values.renderer_debug);
    Read("Debugging", Settings::values.pica_debugging);
    Read("Debugging", Settings::values.dump_command_buffers);
    Read("Debugging", Settings::values.enable_exception_handler);
    Read("Debugging", Settings::values.deterministic_async_operations);
    Read("Debugging", Settings::values.delay_start_for_lle_modules);
    Read("Debugging", Settings::values.toggle_unique_data_console_type);
    Read("Debugging", Settings::values.use_gdbstub);
    Read("Debugging", Settings::values.gdbstub_port);
    Read("Debugging", Settings::values.enable_rpc_server);
    for (const auto& service_module : Service::service_module_map) {
        const bool use_lle = ini->GetBoolean("Debugging", "LLE\\" + service_module.name, false);
        Settings::values.lle_modules[service_module.name] = use_lle;
    }

    // Web service
    Read("WebService", Settings::values.web_api_url);
    Read("WebService", Settings::values.network_token);

    // Frontend
    Read("UWP", values.confirm_before_closing);
    Read("UWP", values.pause_when_in_background);
    Read("UWP", values.mute_when_in_background);
    Read("UWP", values.allow_broadcasting);
    Read("UWP", values.auto_start_broadcasts);
    Read("UWP", values.allow_third_party_broadcast_control);
    values.screenshot_path.SetValue(
        ResolvePortablePath(ini->Get("UWP", values.screenshot_path.GetLabel(),
                                     values.screenshot_path.GetValue(/*need_global=*/true)),
                            stored_data_root));
    Read("UWP", values.game_list_hide_no_icon);
    Read("UWP", values.game_list_single_line_mode);
    Read("UWP", values.game_list_icon_size);
    Read("UWP", values.game_list_row_1);
    Read("UWP", values.game_list_row_2);
    if (values.game_list_row_1.GetValue() < GameListText::FileName ||
        values.game_list_row_1.GetValue() > GameListText::LongTitleName) {
        values.game_list_row_1.SetValue(GameListText::TitleName);
    }
    if (values.game_list_row_2.GetValue() < GameListText::NoText ||
        values.game_list_row_2.GetValue() > GameListText::LongTitleName) {
        values.game_list_row_2.SetValue(GameListText::FileName);
    }
    Read("UWP", values.game_list_favorite_first);
    Read("UWP", values.game_list_favorites_only);
    Read("UWP", values.right_stick_function);
    if (values.right_stick_function.GetValue() < RightStickFunction::CStickAndTouchscreen ||
        values.right_stick_function.GetValue() > RightStickFunction::Touchscreen) {
        values.right_stick_function.SetValue(RightStickFunction::CStickAndTouchscreen);
    }
    Read("UWP", values.touchscreen_cursor_sensitivity);
    values.touchscreen_cursor_sensitivity.SetValue(
        std::clamp(values.touchscreen_cursor_sensitivity.GetValue(), 0.25f, 4.0f));
    Read("UWP", values.touchscreen_cursor_timeout);
    Read("UWP", values.screenscraper_enabled);
    Read("UWP", values.screenscraper_username);
    Read("UWP", values.screenscraper_auto_scrape);
    Read("UWP", values.screenscraper_download_metadata);
    Read("UWP", values.screenscraper_download_box_2d);
    Read("UWP", values.screenscraper_download_box_3d);
    Read("UWP", values.screenscraper_download_box_2d_back);
    Read("UWP", values.screenscraper_download_game_logo);
    Read("UWP", values.screenscraper_download_support_2d);
    Read("UWP", values.screenscraper_download_mix_v1);
    Read("UWP", values.screenscraper_download_mix_v2);
    Read("UWP", values.screenscraper_download_fanart);
    Read("UWP", values.screenscraper_download_screenshot);
    Read("UWP", values.screenscraper_download_video);
    Read("UWP", values.screenscraper_box_art);
    if (ini->HasValue("UWP", values.screenscraper_large_box_art.GetLabel())) {
        Read("UWP", values.screenscraper_large_box_art);
    } else {
        // Default the large-art choice to the box-art choice when the key is absent.
        values.screenscraper_large_box_art.SetValue(values.screenscraper_box_art.GetValue());
    }
    Read("UWP", values.screenscraper_background);
    values.screenscraper_storage.SetValue(
        ResolvePortablePath(ini->Get("UWP", values.screenscraper_storage.GetLabel(),
                                     values.screenscraper_storage.GetValue()),
                            stored_data_root));
    values.screenscraper_storage.SetValue(EnsureConfiguredDirectory(
        values.screenscraper_storage.GetValue(), Storage::DevelopmentScreenScraperPath,
        FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "screenscraper"));
    if (values.screenscraper_box_art.GetValue() < ScreenScraperBoxArt::SmdhIcon ||
        values.screenscraper_box_art.GetValue() > ScreenScraperBoxArt::Support2D) {
        values.screenscraper_box_art.SetValue(ScreenScraperBoxArt::Box2D);
    }
    if (values.screenscraper_large_box_art.GetValue() < ScreenScraperBoxArt::SmdhIcon ||
        values.screenscraper_large_box_art.GetValue() > ScreenScraperBoxArt::Support2D) {
        values.screenscraper_large_box_art.SetValue(ScreenScraperBoxArt::Box2D);
    }
    if (values.screenscraper_background.GetValue() < ScreenScraperBackground::Off ||
        values.screenscraper_background.GetValue() > ScreenScraperBackground::MixRecalboxV2) {
        values.screenscraper_background.SetValue(ScreenScraperBackground::FanArt);
    }
    values.game_directory.SetValue(ResolvePortablePath(
        ini->Get("UWP", values.game_directory.GetLabel(), values.game_directory.GetValue()),
        stored_data_root));
    if (!values.game_directory.GetValue().empty()) {
        values.game_directory.SetValue(EnsureConfiguredDirectory(
            values.game_directory.GetValue(), Storage::DevelopmentApplicationsPath,
            Storage::ActiveGamesPath()));
    }
    Read("UWP", values.game_directory_deep_scan);
    Read("UWP", values.show_advanced_frametime_info);
    Read("UWP", values.show_status_overlay);
    Read("UWP", values.show_filter_bar);
    Read("UWP", values.enable_discord_presence);
    Read("UWP", values.ui_scale);
    values.recent_files.clear();
    const auto recent_count = static_cast<std::size_t>(std::clamp<long>(
        ini->GetInteger("UWP", "recent_file_count", 0), 0, static_cast<long>(MaxRecentFiles)));
    for (std::size_t i = 0; i < recent_count; ++i) {
        const std::string path = ResolvePortablePath(
            ini->Get("UWP", fmt::format("recent_file_{}", i), ""), stored_data_root);
        if (!path.empty()) {
            values.recent_files.emplace_back(path);
        }
    }
    values.favorited_ids.clear();
    const auto favorite_count = static_cast<std::size_t>(std::clamp<long>(
        ini->GetInteger("UWP", "favorite_count", 0), 0, static_cast<long>(MaxFavoriteTitles)));
    for (std::size_t i = 0; i < favorite_count; ++i) {
        const std::string text = ini->Get("UWP", fmt::format("favorite_{}", i), "");
        u64 program_id = 0;
        const auto [end, error] =
            std::from_chars(text.data(), text.data() + text.size(), program_id, 16);
        if (program_id != 0 && error == std::errc{} && end == text.data() + text.size() &&
            std::find(values.favorited_ids.begin(), values.favorited_ids.end(), program_id) ==
                values.favorited_ids.end()) {
            values.favorited_ids.push_back(program_id);
        }
    }
    values.inserted_cartridge.SetValue(ResolvePortablePath(
        ini->Get("UWP", values.inserted_cartridge.GetLabel(), values.inserted_cartridge.GetValue()),
        stored_data_root));
    if (!values.inserted_cartridge.GetValue().empty() &&
        !FileUtil::Exists(values.inserted_cartridge.GetValue())) {
        values.inserted_cartridge.SetValue("");
    }
    for (std::size_t i = 0; i < values.controller_hotkeys.size(); ++i) {
        values.controller_hotkeys[i] =
            ini->Get("UWP", fmt::format("controller_hotkey_{}", i), values.controller_hotkeys[i]);
        const Common::ParamPackage params{values.controller_hotkeys[i]};
        if (!values.controller_hotkeys[i].empty() && !IsSupportedButtonBinding(params)) {
            values.controller_hotkeys[i].clear();
        }
    }
    for (std::size_t i = 0; i < values.in_game_menu_chord.size(); ++i) {
        const std::string key = fmt::format("in_game_menu_chord_{}", i);
        values.in_game_menu_chord[i] = ini->Get("UWP", key.c_str(), values.in_game_menu_chord[i]);
        const Common::ParamPackage params{values.in_game_menu_chord[i]};
        if (!values.in_game_menu_chord[i].empty() && !IsSupportedButtonBinding(params)) {
            values.in_game_menu_chord[i].clear();
        }
    }

    const auto load_path = [&stored_data_root](const char* key, FileUtil::UserPath path,
                                               std::string_view development_fallback) {
        const std::string value = ini->Get("Paths", key, "");
        if (!value.empty()) {
            const std::string selected = ResolvePortablePath(value, stored_data_root);
            const std::string available = EnsureConfiguredDirectory(selected, development_fallback,
                                                                    FileUtil::GetUserPath(path));
            if (!available.empty()) {
                FileUtil::UpdateUserPath(path, available);
            }
        }
    };
    load_path("nand", FileUtil::UserPath::NANDDir, Storage::DevelopmentNandPath);
    load_path("sdmc", FileUtil::UserPath::SDMCDir, Storage::DevelopmentSdmcPath);
    load_path("load", FileUtil::UserPath::LoadDir, Storage::DevelopmentTextureLoadPath);
    const std::string saved_mods = ini->Get("Paths", "mods", "");
    if (!saved_mods.empty()) {
        const std::string selected = ResolvePortablePath(saved_mods, stored_data_root);
        const std::string available =
            EnsureConfiguredDirectory(selected, Storage::DevelopmentModsPath,
                                      FileUtil::GetDefaultUserPath(FileUtil::UserPath::ModsDir));
        if (!available.empty()) {
            FileUtil::UpdateUserPath(FileUtil::UserPath::ModsDir, available);
        }
    }
    load_path("dump", FileUtil::UserPath::DumpDir, Storage::DevelopmentTextureDumpPath);
    load_path("cheats", FileUtil::UserPath::CheatsDir, Storage::DevelopmentCheatsPath);

    EnsureProfileList();
    NormalizeDisplaySettings();
    LOG_INFO(Config, "Loaded {}", ConfigPath());
}

void SaveConfig() {
    EnsureProfileList();
    NormalizeInputSettings();
    NormalizeDisplaySettings();
    auto& settings = Settings::values;
    settings.input_profiles[settings.current_input_profile_index] = settings.current_input_profile;
    for (auto& profile : settings.input_profiles) {
        NormalizeInputSettings(profile);
    }

    std::ostringstream out;

    out << "[Controls]\n";
    WriteRaw(out, "profile_count", std::to_string(settings.input_profiles.size()));
    WriteRaw(out, "current_profile", std::to_string(settings.current_input_profile_index));
    Write(out, Settings::values.use_artic_base_controller);
    for (std::size_t i = 0; i < settings.input_profiles.size(); ++i) {
        WriteProfile(out, i, settings.input_profiles[i]);
    }

    out << "\n[Core]\n";
    Write(out, Settings::values.enable_gamemode);
    Write(out, Settings::values.use_cpu_jit);
    Write(out, Settings::values.cpu_clock_percentage);

    out << "\n[Renderer]\n";
    Write(out, Settings::values.graphics_api);
    Write(out, Settings::values.physical_device);
    Write(out, Settings::values.spirv_shader_gen);
    Write(out, Settings::values.disable_spirv_optimizer);
    Write(out, Settings::values.use_hw_shader);
    Write(out, Settings::values.use_shader_jit);
    Write(out, Settings::values.shaders_accurate_mul);
    Write(out, Settings::values.async_shader_compilation);
    Write(out, Settings::values.async_presentation);
    Write(out, Settings::values.simulate_3ds_gpu_timings);
    Write(out, Settings::values.resolution_factor);
    Write(out, Settings::values.use_disk_shader_cache);
    Write(out, Settings::values.use_vsync);
    Write(out, Settings::values.use_skip_duplicate_frames);
    Write(out, Settings::values.use_display_refresh_rate_detection);
    Write(out, Settings::values.texture_filter);
    Write(out, Settings::values.texture_sampling);
    Write(out, Settings::values.delay_game_render_thread_us);
    Write(out, Settings::values.frame_limit);
    Write(out, Settings::values.turbo_limit);
    Write(out, Settings::values.render_3d);
    Write(out, Settings::values.factor_3d);
    Write(out, Settings::values.mono_render_option);
    Write(out, Settings::values.swap_eyes_3d);
    Write(out, Settings::values.disable_right_eye_render);
    Write(out, Settings::values.filter_mode);
    Write(out, Settings::values.use_integer_scaling);
    Write(out, Settings::values.bg_red);
    Write(out, Settings::values.bg_green);
    Write(out, Settings::values.bg_blue);
    WriteRaw(out, "pp_shader_name", Settings::values.pp_shader_name.GetValue(/*need_global=*/true));

    out << "\n[Layout]\n";
    Write(out, Settings::values.layout_option);
    Write(out, Settings::values.swap_screen);
    Write(out, Settings::values.upright_screen);
    Write(out, Settings::values.screen_gap);
    Write(out, Settings::values.large_screen_proportion);
    Write(out, Settings::values.small_screen_position);
    Write(out, Settings::values.layouts_to_cycle);
    Write(out, Settings::values.aspect_ratio);
    Write(out, Settings::values.screen_top_stretch);
    Write(out, Settings::values.screen_top_leftright_padding);
    Write(out, Settings::values.screen_top_topbottom_padding);
    Write(out, Settings::values.screen_bottom_stretch);
    Write(out, Settings::values.screen_bottom_leftright_padding);
    Write(out, Settings::values.screen_bottom_topbottom_padding);
    Write(out, Settings::values.custom_top_x);
    Write(out, Settings::values.custom_top_y);
    Write(out, Settings::values.custom_top_width);
    Write(out, Settings::values.custom_top_height);
    Write(out, Settings::values.custom_bottom_x);
    Write(out, Settings::values.custom_bottom_y);
    Write(out, Settings::values.custom_bottom_width);
    Write(out, Settings::values.custom_bottom_height);
    Write(out, Settings::values.custom_second_layer_opacity);

    out << "\n[Audio]\n";
    Write(out, Settings::values.audio_emulation);
    Write(out, Settings::values.enable_audio_stretching);
    Write(out, Settings::values.enable_realtime_audio);
    Write(out, Settings::values.volume);
    Write(out, Settings::values.output_type);
    Write(out, Settings::values.output_device);
    Write(out, Settings::values.input_type);
    Write(out, Settings::values.input_device);
    Write(out, Settings::values.simulate_headphones_plugged);

    out << "\n[VideoDumping]\n";
    WriteRaw(out, "output_format", Settings::values.output_format);
    WriteRaw(out, "format_options", Settings::values.format_options);
    WriteRaw(out, "video_encoder", Settings::values.video_encoder);
    WriteRaw(out, "video_encoder_options", Settings::values.video_encoder_options);
    WriteRaw(out, "video_bitrate", std::to_string(Settings::values.video_bitrate));
    WriteRaw(out, "audio_encoder", Settings::values.audio_encoder);
    WriteRaw(out, "audio_encoder_options", Settings::values.audio_encoder_options);
    WriteRaw(out, "audio_bitrate", std::to_string(Settings::values.audio_bitrate));

    out << "\n[Utility]\n";
    Write(out, Settings::values.dump_textures);
    Write(out, Settings::values.custom_textures);
    Write(out, Settings::values.preload_textures);
    Write(out, Settings::values.async_custom_loading);

    out << "\n[Storage]\n";
    Write(out, Settings::values.use_virtual_sd);
    Write(out, Settings::values.use_custom_storage);
    Write(out, Settings::values.compress_cia_installs);
    Write(out, Settings::values.async_fs_operations);

    out << "\n[System]\n";
    Write(out, Settings::values.is_new_3ds);
    Write(out, Settings::values.lle_applets);
    Write(out, Settings::values.enable_required_online_lle_modules);
    Write(out, Settings::values.region_value);
    Write(out, Settings::values.init_clock);
    WriteRaw(out, "init_time", std::to_string(Settings::values.init_time.GetValue()));
    Write(out, Settings::values.init_time_offset);
    Write(out, Settings::values.init_ticks_type);
    Write(out, Settings::values.init_ticks_override);
    Write(out, Settings::values.plugin_loader_enabled);
    Write(out, Settings::values.allow_plugin_loader);
    Write(out, Settings::values.steps_per_hour);
    Write(out, Settings::values.apply_region_free_patch);

    out << "\n[Camera]\n";
    for (int i = 0; i < 3; ++i) {
        WriteRaw(out, fmt::format("camera_{}_name", i), Settings::values.camera_name[i]);
        WriteRaw(out, fmt::format("camera_{}_config", i), Settings::values.camera_config[i]);
        WriteRaw(out, fmt::format("camera_{}_flip", i),
                 std::to_string(Settings::values.camera_flip[i]));
    }

    out << "\n[Miscellaneous]\n";
    WriteRaw(out, "log_filter", Settings::values.log_filter.GetValue());
    Write(out, Settings::values.log_regex_filter);

    out << "\n[Debugging]\n";
    Write(out, Settings::values.instant_debug_log);
    Write(out, Settings::values.renderer_debug);
    Write(out, Settings::values.pica_debugging);
    Write(out, Settings::values.dump_command_buffers);
    Write(out, Settings::values.enable_exception_handler);
    Write(out, Settings::values.deterministic_async_operations);
    Write(out, Settings::values.delay_start_for_lle_modules);
    Write(out, Settings::values.toggle_unique_data_console_type);
    Write(out, Settings::values.use_gdbstub);
    Write(out, Settings::values.gdbstub_port);
    Write(out, Settings::values.enable_rpc_server);
    for (const auto& [name, use_lle] : Settings::values.lle_modules) {
        WriteRaw(out, "LLE\\" + name, use_lle ? "true" : "false");
    }

    out << "\n[WebService]\n";
    Write(out, Settings::values.web_api_url);
    Write(out, Settings::values.network_token);

    out << "\n[UWP]\n";
    Write(out, values.confirm_before_closing);
    Write(out, values.pause_when_in_background);
    Write(out, values.mute_when_in_background);
    Write(out, values.allow_broadcasting);
    Write(out, values.auto_start_broadcasts);
    Write(out, values.allow_third_party_broadcast_control);
    WriteRaw(out, values.screenshot_path.GetLabel(),
             MakePortablePath(values.screenshot_path.GetValue(/*need_global=*/true)));
    Write(out, values.game_list_hide_no_icon);
    Write(out, values.game_list_single_line_mode);
    Write(out, values.game_list_icon_size);
    Write(out, values.game_list_row_1);
    Write(out, values.game_list_row_2);
    Write(out, values.game_list_favorite_first);
    Write(out, values.game_list_favorites_only);
    Write(out, values.right_stick_function);
    Write(out, values.touchscreen_cursor_sensitivity);
    Write(out, values.touchscreen_cursor_timeout);
    Write(out, values.screenscraper_enabled);
    Write(out, values.screenscraper_username);
    Write(out, values.screenscraper_auto_scrape);
    Write(out, values.screenscraper_download_metadata);
    Write(out, values.screenscraper_download_box_2d);
    Write(out, values.screenscraper_download_box_3d);
    Write(out, values.screenscraper_download_box_2d_back);
    Write(out, values.screenscraper_download_game_logo);
    Write(out, values.screenscraper_download_support_2d);
    Write(out, values.screenscraper_download_mix_v1);
    Write(out, values.screenscraper_download_mix_v2);
    Write(out, values.screenscraper_download_fanart);
    Write(out, values.screenscraper_download_screenshot);
    Write(out, values.screenscraper_download_video);
    Write(out, values.screenscraper_box_art);
    Write(out, values.screenscraper_large_box_art);
    Write(out, values.screenscraper_background);
    WriteRaw(out, values.screenscraper_storage.GetLabel(),
             MakePortablePath(values.screenscraper_storage.GetValue()));
    WriteRaw(out, values.game_directory.GetLabel(),
             MakePortablePath(values.game_directory.GetValue()));
    Write(out, values.game_directory_deep_scan);
    Write(out, values.show_advanced_frametime_info);
    Write(out, values.show_status_overlay);
    Write(out, values.show_filter_bar);
    Write(out, values.enable_discord_presence);
    Write(out, values.ui_scale);
    if (values.recent_files.size() > MaxRecentFiles) {
        values.recent_files.resize(MaxRecentFiles);
    }
    WriteRaw(out, "recent_file_count", std::to_string(values.recent_files.size()));
    for (std::size_t i = 0; i < values.recent_files.size(); ++i) {
        WriteRaw(out, fmt::format("recent_file_{}", i), MakePortablePath(values.recent_files[i]));
    }
    std::erase(values.favorited_ids, u64{0});
    std::sort(values.favorited_ids.begin(), values.favorited_ids.end());
    values.favorited_ids.erase(
        std::unique(values.favorited_ids.begin(), values.favorited_ids.end()),
        values.favorited_ids.end());
    if (values.favorited_ids.size() > MaxFavoriteTitles) {
        values.favorited_ids.resize(MaxFavoriteTitles);
    }
    WriteRaw(out, "favorite_count", std::to_string(values.favorited_ids.size()));
    for (std::size_t i = 0; i < values.favorited_ids.size(); ++i) {
        WriteRaw(out, fmt::format("favorite_{}", i),
                 fmt::format("{:016X}", values.favorited_ids[i]));
    }
    WriteRaw(out, values.inserted_cartridge.GetLabel(),
             MakePortablePath(values.inserted_cartridge.GetValue()));
    for (std::size_t i = 0; i < values.controller_hotkeys.size(); ++i) {
        WriteRaw(out, fmt::format("controller_hotkey_{}", i), values.controller_hotkeys[i]);
    }
    for (std::size_t i = 0; i < values.in_game_menu_chord.size(); ++i) {
        WriteRaw(out, fmt::format("in_game_menu_chord_{}", i), values.in_game_menu_chord[i]);
    }

    out << "\n[Paths]\n";
    // data_root keeps paths inside LocalState portable when a configuration is copied.
    WriteRaw(out, "data_root", CurrentDataRoot());
    WriteRaw(out, "nand", MakePortablePath(FileUtil::GetUserPath(FileUtil::UserPath::NANDDir)));
    WriteRaw(out, "sdmc", MakePortablePath(FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir)));
    WriteRaw(out, "load", MakePortablePath(FileUtil::GetUserPath(FileUtil::UserPath::LoadDir)));
    WriteRaw(out, "mods", MakePortablePath(FileUtil::GetUserPath(FileUtil::UserPath::ModsDir)));
    WriteRaw(out, "dump", MakePortablePath(FileUtil::GetUserPath(FileUtil::UserPath::DumpDir)));
    WriteRaw(out, "cheats", MakePortablePath(FileUtil::GetUserPath(FileUtil::UserPath::CheatsDir)));

    const std::string path = ConfigPath();
    FileUtil::CreateFullPath(path);
    if (!FileUtil::WriteStringToFile(true, path, out.str())) {
        LOG_ERROR(Config, "Failed to write {}", path);
        return;
    }
    LOG_DEBUG(Config, "Saved {}", path);
}

std::string GetPerGameConfigPath(u64 title_id, const std::string& application_path) {
    std::string config_name = title_id != 0 ? fmt::format("{:016X}", title_id)
                                            : std::string{FileUtil::GetFilename(application_path)};
    if (config_name.empty()) {
        config_name = "unknown";
    }

    // GetFilename removes directory traversal; replace the characters invalid in a Win32
    // filename.
    for (char& character : config_name) {
        const unsigned char value = static_cast<unsigned char>(character);
        if (value < 0x20 || character == '<' || character == '>' || character == ':' ||
            character == '"' || character == '/' || character == '\\' || character == '|' ||
            character == '?' || character == '*') {
            character = '_';
        }
    }
    while (!config_name.empty() && (config_name.back() == '.' || config_name.back() == ' ')) {
        config_name.pop_back();
    }
    if (config_name.empty()) {
        config_name = "unknown";
    }

    return FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) + "custom/" + config_name + ".ini";
}

bool LoadPerGameConfig(u64 title_id, const std::string& application_path) {
    RestoreGlobalConfig();

    const std::string path = GetPerGameConfigPath(title_id, application_path);
    if (!FileUtil::Exists(path)) {
        LOG_INFO(Config, "No per-application config for {}; using global settings", path);
        return true;
    }

    std::string buffer;
    if (!FileUtil::ReadFileToString(true, path, buffer)) {
        LOG_ERROR(Config, "Failed to read per-application config {}", path);
        return false;
    }
    if (buffer.empty()) {
        LOG_INFO(Config, "Empty per-application config {}; using global settings", path);
        return true;
    }

    const INIReader reader{buffer.c_str(), buffer.size()};
    if (reader.ParseError() != 0) {
        LOG_ERROR(Config, "Per-application config {} is unreadable (line {})", path,
                  reader.ParseError());
        return false;
    }

    ReadPerGameValues(reader);
    NormalizeDisplaySettings();
    LOG_INFO(Config, "Loaded per-application config {}", path);
    return true;
}

bool SavePerGameConfig(u64 title_id, const std::string& application_path) {
    NormalizeDisplaySettings();
    std::ostringstream out;
    WritePerGameValues(out);

    const std::string path = GetPerGameConfigPath(title_id, application_path);
    FileUtil::CreateFullPath(path);
    if (!FileUtil::WriteStringToFile(true, path, out.str())) {
        LOG_ERROR(Config, "Failed to write per-application config {}", path);
        return false;
    }
    LOG_INFO(Config, "Saved per-application config {}", path);
    return true;
}

bool DeletePerGameConfig(u64 title_id, const std::string& application_path) {
    const std::string path = GetPerGameConfigPath(title_id, application_path);
    if (!FileUtil::Exists(path)) {
        return true;
    }
    if (!FileUtil::Delete(path)) {
        LOG_ERROR(Config, "Failed to delete per-application config {}", path);
        return false;
    }
    LOG_INFO(Config, "Deleted per-application config {}", path);
    return true;
}

} // namespace UWP
