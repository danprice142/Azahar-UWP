// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <typeinfo>
#include <utility>
#include <vector>

// Windows.Foundation must come first: it defines the EventHandler<T> template BackRequested uses.
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <SDL.h>
// The entry point lives in sdl_main_winrt.cpp; only SDL_main.h's main redirection is needed.
#define SDL_MAIN_HANDLED
#include <SDL_main.h>

#include <glad/glad.h>

#include <imgui.h>

#include "citra_uwp/emu_window_uwp.h"
#include "citra_uwp/emulation_thread.h"
#include "citra_uwp/keyboard_input.h"
#include "citra_uwp/live_camera.h"
#include "citra_uwp/remote_library_server.h"
#include "citra_uwp/screenscraper.h"
#include "citra_uwp/still_image_camera.h"
#include "citra_uwp/storage_paths.h"
#include "citra_uwp/ui/configure_dialog.h"
#include "citra_uwp/ui/dialogs.h"
#include "citra_uwp/ui/file_browser.h"
#include "citra_uwp/ui/game_list.h"
#include "citra_uwp/ui/imgui_manager.h"
#include "citra_uwp/ui/main_menu.h"
#include "citra_uwp/ui/mii_selector.h"
#include "citra_uwp/ui/multiplayer.h"
#include "citra_uwp/ui/popups.h"
#include "citra_uwp/ui/software_keyboard.h"
#include "citra_uwp/ui/virtual_keyboard.h"
#include "citra_uwp/uwp_config.h"
#include "citra_uwp/uwp_settings.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/param_package.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/zstd_compression.h"
#include "core/core.h"
#include "core/dumping/ffmpeg_backend.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/frontend/input.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/nfc/nfc.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"
#include "core/loader/loader.h"
#include "core/movie.h"
#include "core/savestate.h"
#include "core/system_titles.h"
#include "input_common/main.h"
#include "input_common/udp/udp.h"
#include "network/network.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/video_core.h"

#include <fmt/format.h>
#include <lodepng.h>

// Defined in src/uwp/compat/uwp_jit_memory.cpp, which must not include any Windows headers.
extern "C" int citra_uwp_JitMemorySupported(void);

namespace {

/// Xbox is always full screen and must not receive desktop windowing requests.
void RequestDesktopFullScreen() try {
    if (SDL_WinRTGetDeviceFamily() != SDL_WINRT_DEVICEFAMILY_DESKTOP) {
        return;
    }

    using winrt::Windows::UI::ViewManagement::ApplicationView;
    using winrt::Windows::UI::ViewManagement::ApplicationViewWindowingMode;
    ApplicationView::PreferredLaunchWindowingMode(ApplicationViewWindowingMode::FullScreen);
    const ApplicationView view = ApplicationView::GetForCurrentView();
    if (!view.IsFullScreenMode()) {
        view.TryEnterFullScreenMode();
    }
} catch (...) {
    // Full-screen presentation is a best-effort request on desktop.
}

/// Must run before logging is initialised, which opens a file under the user directory.
bool SetupUserDirectory() try {
    if (!UWP::Storage::InitializeUserDirectory()) {
        return false;
    }

    for (const auto path :
         {FileUtil::UserPath::SysDataDir, FileUtil::UserPath::NANDDir, FileUtil::UserPath::SDMCDir,
          FileUtil::UserPath::ConfigDir, FileUtil::UserPath::LogDir, FileUtil::UserPath::StatesDir,
          FileUtil::UserPath::CacheDir, FileUtil::UserPath::CheatsDir, FileUtil::UserPath::LoadDir,
          FileUtil::UserPath::ModsDir, FileUtil::UserPath::DumpDir, FileUtil::UserPath::ShaderDir,
          FileUtil::UserPath::IconsDir}) {
        FileUtil::CreateFullPath(FileUtil::GetUserPath(path));
    }

    return true;
} catch (...) {
    return false;
}

struct DefaultStoragePaths {
    std::string_view applications;
    std::string_view nand;
    std::string_view sdmc;
    std::string_view texture_load;
    std::string_view mods;
    std::string_view texture_dump;
    std::string_view cheats;
    std::string_view screenscraper;
};

constexpr DefaultStoragePaths DevelopmentStoragePaths{
    UWP::Storage::DevelopmentApplicationsPath, UWP::Storage::DevelopmentNandPath,
    UWP::Storage::DevelopmentSdmcPath,         UWP::Storage::DevelopmentTextureLoadPath,
    UWP::Storage::DevelopmentModsPath,         UWP::Storage::DevelopmentTextureDumpPath,
    UWP::Storage::DevelopmentCheatsPath,       UWP::Storage::DevelopmentScreenScraperPath,
};

constexpr DefaultStoragePaths ExternalStoragePaths{
    UWP::Storage::ExternalApplicationsPath, UWP::Storage::ExternalNandPath,
    UWP::Storage::ExternalSdmcPath,         UWP::Storage::ExternalTextureLoadPath,
    UWP::Storage::ExternalModsPath,         UWP::Storage::ExternalTextureDumpPath,
    UWP::Storage::ExternalCheatsPath,       UWP::Storage::ExternalScreenScraperPath,
};

bool PrepareStoragePaths(const DefaultStoragePaths& paths) {
    bool prepared = true;
    for (const std::string_view path :
         {paths.applications, paths.nand, paths.sdmc, paths.texture_load, paths.mods,
          paths.texture_dump, paths.cheats, paths.screenscraper}) {
        std::string error;
        if (!UWP::Storage::EnsureWritableDirectory(path, error)) {
            prepared = false;
        }
    }
    return prepared;
}

const DefaultStoragePaths& SelectDefaultStoragePaths() {
    const bool external_detected =
        UWP::Storage::IsPathDriveAvailable(UWP::Storage::ExternalApplicationsPath);
    if (external_detected && PrepareStoragePaths(ExternalStoragePaths)) {
        return ExternalStoragePaths;
    }

    PrepareStoragePaths(DevelopmentStoragePaths);
    return DevelopmentStoragePaths;
}

void ApplyFirstBootStoragePaths() {
    const DefaultStoragePaths& defaults = SelectDefaultStoragePaths();
    UWP::values.game_directory.SetValue(std::string{defaults.applications});
    FileUtil::UpdateUserPath(FileUtil::UserPath::NANDDir, std::string{defaults.nand});
    FileUtil::UpdateUserPath(FileUtil::UserPath::SDMCDir, std::string{defaults.sdmc});
    FileUtil::UpdateUserPath(FileUtil::UserPath::LoadDir, std::string{defaults.texture_load});
    FileUtil::UpdateUserPath(FileUtil::UserPath::ModsDir, std::string{defaults.mods});
    FileUtil::UpdateUserPath(FileUtil::UserPath::DumpDir, std::string{defaults.texture_dump});
    FileUtil::UpdateUserPath(FileUtil::UserPath::CheatsDir, std::string{defaults.cheats});
    UWP::values.screenscraper_storage.SetValue(std::string{defaults.screenscraper});
    Settings::values.use_custom_storage.SetValue(true);
}

/// Marks the system back request (B on Xbox) as handled so the shell does not close the app.
void InstallBackRequestHandler() try {
    using winrt::Windows::UI::Core::SystemNavigationManager;
    SystemNavigationManager::GetForCurrentView().BackRequested(
        [](const winrt::Windows::Foundation::IInspectable&,
           const winrt::Windows::UI::Core::BackRequestedEventArgs& args) { args.Handled(true); });
} catch (const std::exception& ex) {
    LOG_WARNING(Frontend, "Could not install the back-request handler: {}", ex.what());
}

void LaunchExternalUri(std::wstring_view uri, std::string& error_out) try {
    using winrt::Windows::Foundation::AsyncStatus;
    using winrt::Windows::Foundation::Uri;
    using winrt::Windows::System::Launcher;

    auto operation = Launcher::LaunchUriAsync(Uri{uri});
    operation.Completed([](const auto& completed, AsyncStatus status) {
        if (status == AsyncStatus::Completed && !completed.GetResults()) {
            LOG_WARNING(Frontend, "The Xbox shell declined an external URI launch");
        }
    });
} catch (const std::exception& ex) {
    error_out = std::string{"Unable to open the link: "} + ex.what();
}

/// Seeds default bindings from the first game controller. Retried every frame because WGI
/// reports the controller shortly after launch; user-captured bindings are never overwritten.
void SeedDefaultInputBindings() {
    static bool seeded = false;
    if (seeded) {
        return;
    }

    auto& profile = Settings::values.current_input_profile;
    const auto is_bound = [](const std::string& binding) {
        return !UWP::KeyboardInput::ButtonBinding(binding).empty();
    };
    if (std::any_of(profile.buttons.begin(), profile.buttons.end(), is_bound) ||
        std::any_of(profile.analogs.begin(), profile.analogs.end(), is_bound)) {
        seeded = true;
        return;
    }

    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) {
            continue;
        }

        char guid[64];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));

        const auto base = [&] {
            Common::ParamPackage params{{"engine", "sdl"}, {"api", "controller"}};
            params.Set("guid", std::string{guid});
            params.Set("port", 0);
            return params;
        };
        const auto button = [&](int controller_button) {
            auto params = base();
            params.Set("button", controller_button);
            return params.Serialize();
        };
        const auto trigger = [&](int controller_axis) {
            auto params = base();
            params.Set("axis", controller_axis);
            params.Set("direction", "+");
            params.Set("threshold", "0.5");
            return params.Serialize();
        };
        const auto stick = [&](int axis_x, int axis_y) {
            auto params = base();
            params.Set("axis_x", axis_x);
            params.Set("axis_y", axis_y);
            return params.Serialize();
        };

        decltype(profile.buttons) buttons{};
        buttons[Settings::NativeButton::A] = button(SDL_CONTROLLER_BUTTON_A);
        buttons[Settings::NativeButton::B] = button(SDL_CONTROLLER_BUTTON_B);
        buttons[Settings::NativeButton::X] = button(SDL_CONTROLLER_BUTTON_X);
        buttons[Settings::NativeButton::Y] = button(SDL_CONTROLLER_BUTTON_Y);
        buttons[Settings::NativeButton::Up] = button(SDL_CONTROLLER_BUTTON_DPAD_UP);
        buttons[Settings::NativeButton::Down] = button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        buttons[Settings::NativeButton::Left] = button(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        buttons[Settings::NativeButton::Right] = button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        buttons[Settings::NativeButton::L] = button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        buttons[Settings::NativeButton::R] = button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        buttons[Settings::NativeButton::ZL] = trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        buttons[Settings::NativeButton::ZR] = trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        buttons[Settings::NativeButton::Start] = button(SDL_CONTROLLER_BUTTON_START);
        buttons[Settings::NativeButton::Select] = button(SDL_CONTROLLER_BUTTON_BACK);
        buttons[Settings::NativeButton::Home] = button(SDL_CONTROLLER_BUTTON_GUIDE);
        for (std::size_t button_index = 0; button_index < buttons.size(); ++button_index) {
            if (!buttons[button_index].empty()) {
                UWP::KeyboardInput::SetButtonBinding(profile.buttons[button_index],
                                                     buttons[button_index]);
            }
        }
        UWP::KeyboardInput::SetAnalogBinding(
            profile.analogs[Settings::NativeAnalog::CirclePad],
            stick(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY));
        UWP::KeyboardInput::SetAnalogBinding(
            profile.analogs[Settings::NativeAnalog::CStick],
            stick(SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY));

        LOG_INFO(Frontend, "Seeded default input bindings from controller {}", guid);
        seeded = true;
        UWP::SaveConfig();
        return;
    }
}

/// A background file operation (CIA install, ROM compression) polled by the UI thread.
struct FileJob {
    std::thread worker;
    std::atomic<bool> done{false};
    std::atomic<std::size_t> progress{0};
    std::atomic<std::size_t> total{0};
    std::string label;
    std::string message; ///< Written by the worker, read by the UI only after `done`.
    bool active = false;

    ~FileJob() {
        Finish();
    }

    void Start(std::string job_label, std::function<std::string(FileJob&)> fn) {
        label = std::move(job_label);
        done = false;
        progress = 0;
        total = 0;
        active = true;
        worker = std::thread([this, fn = std::move(fn)] {
            message = fn(*this);
            done = true;
        });
    }

    void Finish() {
        if (worker.joinable()) {
            worker.join();
        }
        active = false;
    }
};

/// Captures the current frame as a PNG. The buffer must outlive the asynchronous request.
bool CaptureScreenshot(Core::System& system, const std::string& directory,
                       const std::string& title_path, std::string& error_out) {
    if (!system.IsPoweredOn()) {
        error_out = "No application is running.";
        return false;
    }

    // The scale factor lives on the renderer, not on VideoCore, and 0 means "match the window".
    auto& renderer = system.GPU().Renderer();
    u32 res_scale = Settings::values.resolution_factor.GetValue();
    if (res_scale == 0) {
        res_scale = renderer.GetResolutionScaleFactor();
    }
    const Layout::FramebufferLayout layout = Layout::FrameLayoutFromResolutionScale(res_scale);
    const auto width = static_cast<std::size_t>(layout.width);
    const auto height = static_cast<std::size_t>(layout.height);
    if (width == 0 || height == 0) {
        error_out = "The renderer returned an invalid screenshot size.";
        return false;
    }

    auto pixels = std::make_shared<std::vector<u8>>(width * height * 4);

    std::filesystem::path output_directory =
        directory.empty()
            ? std::filesystem::path{FileUtil::GetUserPath(FileUtil::UserPath::UserDir)} /
                  "screenshots"
            : std::filesystem::path{directory};
    const std::string output_directory_string = output_directory.generic_string() + '/';
    if (!FileUtil::IsDirectory(output_directory_string) &&
        !FileUtil::CreateFullPath(output_directory_string)) {
        error_out = "Could not create screenshot directory " + output_directory.string();
        return false;
    }

    std::string title = std::filesystem::path(title_path).stem().string();
    if (title.empty()) {
        title = "azahar";
    }
    for (char& c : title) {
        if (c == '\\' || c == '/' || c == ':' || c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '|') {
            c = '_';
        }
    }
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    const std::string path =
        (output_directory / fmt::format("{}_{}.png", title, timestamp)).string();

    renderer.RequestScreenshot(
        pixels->data(),
        [pixels, path, width, height](bool invert_y) {
            std::vector<u8> image = *pixels;
            if (invert_y) {
                const std::size_t stride = width * 4;
                for (std::size_t row = 0; row < height / 2; ++row) {
                    std::swap_ranges(image.begin() + row * stride,
                                     image.begin() + (row + 1) * stride,
                                     image.begin() + (height - 1 - row) * stride);
                }
            }

            std::vector<u8> png;
            const unsigned error = lodepng::encode(png, image, static_cast<unsigned>(width),
                                                   static_cast<unsigned>(height));
            if (error != 0) {
                LOG_ERROR(Frontend, "Failed to encode screenshot: {}", lodepng_error_text(error));
                return;
            }
            FileUtil::IOFile file(path, "wb");
            if (!file.IsOpen() || file.WriteBytes(png.data(), png.size()) != png.size()) {
                LOG_ERROR(Frontend, "Failed to write screenshot to {}", path);
                return;
            }
            LOG_INFO(Frontend, "Screenshot saved to {}", path);
        },
        layout);
    return true;
}

bool BootTitle(Core::System& system, Frontend::EmuWindow& emu_window, const std::string& path,
               u64 program_id_hint, std::string& error_out) {
    if (system.IsPoweredOn()) {
        system.Shutdown();
    }
    UWP::RestoreGlobalConfig();

    auto loader = Loader::GetLoader(path);
    u64 title_id = program_id_hint;
    if (loader) {
        u64 loader_title_id{};
        loader->ReadProgramId(loader_title_id);
        if (loader_title_id != 0) {
            title_id = loader_title_id;
        }
        // Artic and other one-shot loaders cannot be constructed a second time by System::Load.
        if (!loader->SupportsMultipleInstancesForSameFile()) {
            system.RegisterAppLoaderEarly(loader);
        }
    }
    if (!UWP::LoadPerGameConfig(title_id, path)) {
        error_out = "The per-application configuration file could not be read.";
        UWP::RestoreGlobalConfig();
        return false;
    }
    UWP::NormalizeDisplaySettings();
    system.ApplySettings();

    Settings::LogSettings();

    // Eject first so clearing the saved selection cannot leave the previous cartridge inserted.
    system.EjectCartridge();
    const std::string inserted_cartridge = UWP::values.inserted_cartridge.GetValue();
    if (!inserted_cartridge.empty()) {
        system.InsertCartridge(inserted_cartridge);
    }

    const Core::System::ResultStatus result = system.Load(emu_window, path);
    if (result == Core::System::ResultStatus::Success) {
        LOG_INFO(Frontend, "Booted {}", path);
        return true;
    }

    switch (result) {
    case Core::System::ResultStatus::ErrorGetLoader:
        error_out = "Unsupported or unreadable file.";
        break;
    case Core::System::ResultStatus::ErrorSystemMode:
        error_out = "Could not determine the system mode for this title.";
        break;
    case Core::System::ResultStatus::ErrorLoader_ErrorEncrypted:
        error_out = "The title is encrypted. Azahar cannot boot encrypted dumps.";
        break;
    case Core::System::ResultStatus::ErrorLoader_ErrorInvalidFormat:
        error_out = "The file is not in a format Azahar recognises.";
        break;
    case Core::System::ResultStatus::ErrorLoader_ErrorGbaTitle:
        error_out = "GBA titles are not supported.";
        break;
    case Core::System::ResultStatus::ErrorSystemFiles:
        error_out = "Required system files are missing. Set them up from File > Set Up System "
                    "Files.";
        break;
    case Core::System::ResultStatus::ErrorN3DSApplication:
        error_out = "This title requires New 3DS mode, which is disabled in System settings.";
        break;
    default:
        error_out = "Failed to load the title.";
        break;
    }
    LOG_ERROR(Frontend, "Boot failed for {}: {}", path, error_out);
    UWP::RestoreGlobalConfig();
    return false;
}

/// H.264 requires even dimensions: automatic sizes round up, explicit odd sizes round down.
Layout::FramebufferLayout FitBroadcastLayout(Layout::FramebufferLayout layout, u16 requested_width,
                                             u16 requested_height) {
    const u32 source_width = layout.width;
    const u32 source_height = layout.height;
    if (source_width == 0 || source_height == 0) {
        return {};
    }

    const bool automatic = requested_width == 0 && requested_height == 0;
    u32 target_width = automatic ? (source_width + 1U) & ~1U : requested_width & ~1U;
    u32 target_height = automatic ? (source_height + 1U) & ~1U : requested_height & ~1U;
    target_width = std::max(2U, target_width);
    target_height = std::max(2U, target_height);

    u32 content_width = target_width;
    u32 content_height = static_cast<u32>(std::max<u64>(
        1, (static_cast<u64>(source_height) * target_width + source_width / 2) / source_width));
    if (content_height > target_height) {
        content_height = target_height;
        content_width = static_cast<u32>(
            std::max<u64>(1, (static_cast<u64>(source_width) * target_height + source_height / 2) /
                                 source_height));
    }
    content_width = std::min(content_width, target_width);
    content_height = std::min(content_height, target_height);
    const u32 offset_x = (target_width - content_width) / 2;
    const u32 offset_y = (target_height - content_height) / 2;

    const auto map_x = [&](u32 value) {
        return offset_x +
               static_cast<u32>((static_cast<u64>(value) * content_width + source_width / 2) /
                                source_width);
    };
    const auto map_y = [&](u32 value) {
        return offset_y +
               static_cast<u32>((static_cast<u64>(value) * content_height + source_height / 2) /
                                source_height);
    };
    const auto map_rectangle = [&](Common::Rectangle<u32>& rectangle) {
        rectangle = {map_x(rectangle.left), map_y(rectangle.top), map_x(rectangle.right),
                     map_y(rectangle.bottom)};
    };

    map_rectangle(layout.top_screen);
    map_rectangle(layout.bottom_screen);
    map_rectangle(layout.additional_screen);
    layout.cardboard.top_screen_right_eye = map_x(layout.cardboard.top_screen_right_eye);
    layout.cardboard.bottom_screen_right_eye = map_x(layout.cardboard.bottom_screen_right_eye);
    layout.cardboard.user_x_shift = static_cast<s32>(
        static_cast<s64>(layout.cardboard.user_x_shift) * content_width / source_width);
    layout.width = target_width;
    layout.height = target_height;
    return layout;
}

/// Declared before the window/UI objects in main so its destructor runs after theirs.
struct RuntimeLifetime {
    bool logging_started = false;
    bool input_started = false;
    bool network_started = false;

    ~RuntimeLifetime() {
        if (network_started) {
            Network::Shutdown();
        }
        if (input_started) {
            UWP::KeyboardInput::Shutdown();
            InputCommon::Shutdown();
        }
        if (logging_started) {
            Common::Log::Stop();
        }
        SDL_Quit();
    }
};

} // namespace

int main(int argc, char** argv) {
    RequestDesktopFullScreen();

    // SDL's WinRT bootstrap passes a file-activation path as argv[1] and frees it after main.
    const std::string startup_activation_path =
        argc > 1 && argv != nullptr && argv[argc - 1] != nullptr ? argv[argc - 1] : "";

    // Windows.Gaming.Input is the only supported gamepad API on Xbox; SDL's XInput fallback
    // re-enters WGI mid-dispatch.
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");

    // SDL_INIT_VIDEO is left to EmuWindow_UWP, which sequences it against GL context creation.
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) < 0) {
        return -1;
    }
    RuntimeLifetime runtime;

    if (!SetupUserDirectory()) {
        return -1;
    }

    Settings::values.log_filter = "*:Info";

    const bool config_exists =
        FileUtil::Exists(FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) + "config.ini");
    UWP::LoadConfig();
    // On first boot, storage paths default to E: when available, otherwise to DevelopmentFiles.
    if (!config_exists) {
        ApplyFirstBootStoragePaths();
        UWP::SaveConfig();
    }
    // Profiles gain the desktop frontend's default keys once; keys cleared later stay cleared.
    if (!UWP::values.keyboard_defaults_applied.GetValue()) {
        for (auto& profile : Settings::values.input_profiles) {
            UWP::KeyboardInput::ApplyDefaultKeys(profile, false);
        }
        UWP::KeyboardInput::ApplyDefaultKeys(Settings::values.current_input_profile, false);
        UWP::values.keyboard_defaults_applied.SetValue(true);
        UWP::SaveConfig();
    }

    // Only OpenGL is available on this target.
    if (Settings::values.graphics_api.GetValue() != Settings::GraphicsAPI::OpenGL) {
        Settings::values.graphics_api.SetValue(Settings::GraphicsAPI::OpenGL);
        UWP::SaveConfig();
    }

    UWP::NormalizeDisplaySettings();

    Common::Log::Initialize();
    Common::Log::Start();
    runtime.logging_started = true;

    // The JIT needs the kernel32 VirtualAlloc/VirtualProtect exports (see uwp_jit_memory.cpp).
    if (!citra_uwp_JitMemorySupported()) {
        LOG_CRITICAL(Frontend,
                     "The dynarmic JIT cannot allocate executable memory in this app container");
    }

    // Service::Init indexes lle_modules with unordered_map::at, so seed an entry per module.
    for (const auto& service_module : Service::service_module_map) {
        Settings::values.lle_modules.emplace(service_module.name, false);
    }

    // TouchFromButtonDevice indexes touch_from_button_maps unchecked; guarantee one entry.
    Settings::values.touch_from_button_maps.emplace_back(
        Settings::TouchFromButtonMap{"default", {}});

    InputCommon::Init();
    UWP::KeyboardInput::Init();
    runtime.input_started = true;

    LOG_INFO(Frontend, "Azahar UWP starting");

    Core::System& system = Core::System::GetInstance();

    Frontend::RegisterDefaultApplets(system);
    auto software_keyboard = std::make_shared<UWP::SoftwareKeyboardUwp>();
    system.RegisterSoftwareKeyboard(software_keyboard);
    auto mii_selector = std::make_shared<UWP::MiiSelectorUwp>();
    system.RegisterMiiSelector(mii_selector);

    Camera::RegisterFactory("image", std::make_unique<UWP::StillImageCameraFactory>());
    Camera::RegisterFactory("uwp", std::make_unique<UWP::LiveCameraFactory>());

    UWP::EmuWindow_UWP emu_window{system};
    if (!emu_window.IsValid()) {
        LOG_CRITICAL(Frontend, "Failed to bring up the emu window; aborting");
        return -1;
    }
    if (!UWP::InitializeLiveCameraUiDispatcher()) {
        LOG_WARNING(Service_CAM,
                    "Live camera permission prompts may be unavailable for this launch");
    }

    UWP::ImGuiManager imgui;
    if (!imgui.Initialize(emu_window.GetSDLWindow())) {
        LOG_CRITICAL(Frontend, "Failed to initialise the UI");
        return -1;
    }

    // Renderer and core work run on a shared context owned by the worker.
    emu_window.SetOpenGLPresentationUsesRenderContext(false);
    auto emulation_context = emu_window.CreateSharedContext();
    if (!emulation_context) {
        LOG_CRITICAL(Frontend, "Failed to create the emulation graphics context");
        return -1;
    }
    UWP::EmulationThread emulation_thread{system, std::move(emulation_context)};
    if (!emulation_thread.Start(false)) {
        LOG_CRITICAL(Frontend, "Failed to start the emulation worker");
        return -1;
    }

    const bool network_initialized = Network::Init();
    runtime.network_started = network_initialized;
    if (!network_initialized) {
        LOG_ERROR(Network, "Failed to initialize ENet; multiplayer will remain unavailable");
    }

    // Must run on the thread owning the CoreWindow, i.e. after SDL has created it.
    InstallBackRequestHandler();

    UWP::ConfigureDialog configure;
    UWP::MainMenu main_menu;
    UWP::GameList game_list;
    UWP::Multiplayer multiplayer;
    UWP::MovieDialog movie_dialog;
    UWP::DumpingDialog dumping_dialog;
    UWP::CheatsDialog cheats_dialog;
    UWP::AboutDialog about_dialog;
    UWP::LoadingScreen loading_screen;
    main_menu.SetRecentFiles(UWP::values.recent_files);
    game_list.Refresh();
    UWP::RemoteLibraryServer remote_library_server;
    if (!remote_library_server.Start(26761,
                                     [&game_list] { return game_list.GetRemoteSnapshot(); })) {
        LOG_ERROR(Frontend, "Remote library service failed to start on port 26761");
    }
    UWP::SecondaryScreenWindow secondary_screen;

    std::mutex remote_launch_mutex;
    std::optional<std::string> remote_launch_path;
    std::mutex remote_select_mutex;
    std::optional<u64> remote_select_token;
    std::optional<UWP::RemoteSelection> last_remote_selection;
    std::chrono::steady_clock::time_point last_remote_selection_sent{};
    std::mutex remote_stream_mutex;
    std::vector<InputCommon::CemuhookUDP::RemoteStreamCommand> remote_stream_commands;
    std::shared_ptr<VideoDumper::FFmpegBackend> remote_main_stream;
    std::shared_ptr<VideoDumper::FFmpegBackend> remote_aux_stream;
    std::atomic_bool remote_stream_capabilities_enabled{
        UWP::values.allow_broadcasting.GetValue() &&
        UWP::values.allow_third_party_broadcast_control.GetValue()};
    InputCommon::CemuhookUDP::SetRemoteFrontendCallbacks({
        [&game_list] {
            const auto source = game_list.GetRemoteSnapshot();
            InputCommon::CemuhookUDP::RemoteLibrarySnapshot snapshot;
            snapshot.revision = source.revision;
            snapshot.entries.reserve(source.entries.size());
            for (const auto& entry : source.entries) {
                snapshot.entries.push_back({
                    entry.token,
                    entry.program_id,
                    entry.icon_key,
                    static_cast<u8>((entry.installed ? 1 : 0) | (entry.favorite ? 2 : 0)),
                    entry.title,
                    entry.long_title,
                    entry.file_type,
                    entry.region,
                });
            }
            return snapshot;
        },
        [&game_list](u32 revision,
                     u64 icon_key) -> std::optional<InputCommon::CemuhookUDP::RemoteIcon> {
            const auto icon = game_list.GetRemoteIcon(revision, icon_key);
            if (!icon) {
                return std::nullopt;
            }
            return InputCommon::CemuhookUDP::RemoteIcon{icon_key, static_cast<u16>(icon->width),
                                                        static_cast<u16>(icon->height),
                                                        icon->rgb565};
        },
        [&game_list, &remote_launch_mutex, &remote_launch_path](u32 revision, u64 token) {
            std::string path;
            switch (game_list.ResolveRemoteLaunch(revision, token, path)) {
            case UWP::RemoteLaunchResult::StaleRevision:
                return InputCommon::CemuhookUDP::RemoteLaunchStatus::StaleRevision;
            case UWP::RemoteLaunchResult::UnknownEntry:
                return InputCommon::CemuhookUDP::RemoteLaunchStatus::UnknownEntry;
            case UWP::RemoteLaunchResult::Accepted:
                break;
            }
            std::lock_guard lock{remote_launch_mutex};
            if (remote_launch_path) {
                return InputCommon::CemuhookUDP::RemoteLaunchStatus::Busy;
            }
            remote_launch_path = std::move(path);
            return InputCommon::CemuhookUDP::RemoteLaunchStatus::Accepted;
        },
        [&remote_stream_capabilities_enabled, &remote_library_server] {
            constexpr u32 main_stream_capability = 1U << 4;
            constexpr u32 aux_stream_capability = 1U << 5;
            constexpr u32 catalogue_capability = 1U << 13;
            constexpr u32 media_capability = 1U << 14;
            constexpr u32 selection_capability = 1U << 15;
            u32 capabilities = remote_stream_capabilities_enabled.load(std::memory_order_relaxed)
                                   ? main_stream_capability | aux_stream_capability
                                   : 0;
            capabilities |= selection_capability;
            if (remote_library_server.IsRunning()) {
                capabilities |= catalogue_capability | media_capability;
            }
            return capabilities;
        },
        [&remote_stream_mutex,
         &remote_stream_commands](InputCommon::CemuhookUDP::RemoteStreamCommand command) {
            std::lock_guard lock{remote_stream_mutex};
            remote_stream_commands.emplace_back(std::move(command));
        },
        [&remote_select_mutex, &remote_select_token](u32, u64 token) {
            std::lock_guard lock{remote_select_mutex};
            remote_select_token = token;
        },
    });
    struct RemoteFrontendRegistration {
        ~RemoteFrontendRegistration() {
            InputCommon::CemuhookUDP::SetRemoteFrontendCallbacks({});
        }
    } remote_frontend_registration;
    InputCommon::CemuhookUDP::SetRemoteFrontendState({});

    bool title_running = false;
    bool emulation_paused = false;
    bool app_in_background = false;
    bool auto_paused_in_background = false;
    bool auto_muted_in_background = false;
    bool exit_confirmation_pending = false;
    bool in_game_menu_visible = false;
    bool in_game_menu_popup_open = false;
    int menu_focus_frames = 0;
    UWP::MenuAction menu_return_action = UWP::MenuAction::None;
    bool menu_return_to_quick_menu = false;
    std::string current_title_path;
    u64 current_title_id = 0;
    std::array<bool, Core::SaveStateSlotCount> available_save_state_slots{};
    std::string boot_error;
    bool startup_storage_notice_pending = UWP::Storage::StartupStatusNeedsAttention();
    std::string deferred_startup_activation_path;
    struct PendingIncompatibleStateLoad {
        u32 slot = 0;
        std::string build_name;
    };
    std::optional<PendingIncompatibleStateLoad> pending_incompatible_state_load;
    UWP::DiscordPresencePopup discord_presence;
    std::atomic_bool movie_playback_completed{false};
    system.Movie().SetPlaybackCompletionCallback(
        [&movie_playback_completed] { movie_playback_completed.store(true); });

    // File-picking menu actions share one browser; this remembers why it was opened.
    UWP::FileBrowser file_browser;
    enum class FileAction { None, LoadFile, InstallCIA, LoadAmiibo, Compress, Decompress };
    FileAction pending_file_action = FileAction::None;
    FileJob file_job;

    bool artic_popup_pending = false;
    bool artic_popup_open = false;
    std::array<char, 128> artic_addr{};
    bool sysfiles_popup_pending = false;
    bool sysfiles_popup_open = false;
    bool sysfiles_reinstall_confirmation_pending = false;
    std::array<char, 128> sysfiles_addr{};
    int sysfiles_mode = 0;
    std::pair<bool, bool> sysfiles_installed{false, false};

    std::optional<UWP::MovieDialog::RecordRequest> queued_movie_record;
    std::optional<std::string> queued_movie_playback;

    struct BootOutcome {
        bool success = false;
        std::string error;
        u64 title_id = 0;
        bool movie_recording = false;
        bool movie_playing = false;
    };
    struct PendingBoot {
        std::string path;
        std::future<BootOutcome> outcome;
    };
    std::optional<PendingBoot> pending_boot;
    bool preparing_loaded_title = false;
    bool auto_start_requested_for_title = false;
    // A remote launch may request streaming independently of the local auto-broadcast setting.
    bool remote_launch_stream_requested_for_title = false;
    u32 next_broadcast_authorization_token =
        static_cast<u32>(std::chrono::steady_clock::now().time_since_epoch().count());
    u32 pending_broadcast_authorization_token = 0;
    u8 pending_broadcast_stream_mask = 0;
    std::chrono::steady_clock::time_point pending_broadcast_authorization_deadline{};

    const auto refresh_save_state_slots = [&] {
        available_save_state_slots.fill(false);
        if (!title_running || !system.IsPoweredOn() || current_title_id == 0) {
            return;
        }
        const auto states =
            Core::ListSaveStates(current_title_id, system.Movie().GetCurrentMovieID());
        for (const auto& state : states) {
            if (state.slot < Core::SaveStateSlotCount) {
                available_save_state_slots[state.slot] = true;
            }
        }
    };

    const auto active_broadcast_mask = [&] {
        u8 mask = 0;
        if (remote_main_stream && remote_main_stream->IsDumping()) {
            mask |= 1U << 0;
        }
        if (remote_aux_stream && remote_aux_stream->IsDumping()) {
            mask |= 1U << 1;
        }
        return mask;
    };

    const auto request_broadcast_start = [&](u8 stream_mask) {
        stream_mask &= 0x03;
        if (stream_mask == 0 || !title_running || preparing_loaded_title || !system.IsPoweredOn() ||
            !UWP::values.allow_broadcasting.GetValue()) {
            return false;
        }
        if (++next_broadcast_authorization_token == 0) {
            ++next_broadcast_authorization_token;
        }
        pending_broadcast_authorization_token = next_broadcast_authorization_token;
        pending_broadcast_stream_mask = stream_mask;
        pending_broadcast_authorization_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{10};
        InputCommon::CemuhookUDP::QueueRemoteBroadcastControlRequest(
            true, stream_mask, pending_broadcast_authorization_token);
        return true;
    };

    const auto request_broadcast_stop = [&](u8 stream_mask = 0x03) {
        stream_mask &= 0x03;
        if (stream_mask == 0) {
            return;
        }
        if (++next_broadcast_authorization_token == 0) {
            ++next_broadcast_authorization_token;
        }
        InputCommon::CemuhookUDP::QueueRemoteBroadcastControlRequest(
            false, stream_mask, next_broadcast_authorization_token);
    };

    // Must run on the emulation worker with its GL context current.
    const auto stop_broadcasts_exclusive = [&] {
        if (remote_aux_stream) {
            if (system.IsPoweredOn()) {
                system.GPU().Renderer().StopAuxVideoDumping();
            }
            system.RegisterAuxVideoDumper(nullptr);
            if (remote_aux_stream->IsDumping()) {
                remote_aux_stream->StopDumping();
            }
            remote_aux_stream.reset();
            InputCommon::CemuhookUDP::QueueRemoteStreamStatus(0, 0, 4, 1, "");
        }
        if (remote_main_stream) {
            if (remote_main_stream->IsDumping()) {
                remote_main_stream->StopDumping();
            }
            system.RegisterVideoDumper(nullptr);
            remote_main_stream.reset();
            InputCommon::CemuhookUDP::QueueRemoteStreamStatus(0, 0, 4, 0, "");
        }
    };

    const auto stop_active_broadcasts = [&] {
        const u8 stream_mask =
            static_cast<u8>((active_broadcast_mask() | pending_broadcast_stream_mask) & 0x03);
        if (stream_mask != 0) {
            request_broadcast_stop(stream_mask);
        }
        pending_broadcast_authorization_token = 0;
        pending_broadcast_stream_mask = 0;
        if (!remote_main_stream && !remote_aux_stream) {
            return;
        }
        const bool resume_after_stop = emulation_thread.IsRunning();
        emulation_thread.Pause();
        emulation_thread.Synchronize();
        emulation_thread.RunExclusive(stop_broadcasts_exclusive);
        if (resume_after_stop) {
            emulation_thread.Resume();
        }
    };

    const auto stop_title = [&](bool refresh_library = true) {
        // Send the stop request before clearing the remote state so the receiver can close.
        if (title_running) {
            request_broadcast_stop();
        }
        game_list.StopPlayTimeTracking();
        emulation_thread.PauseAndWait();
        emulation_thread.RunExclusive([&] {
            stop_broadcasts_exclusive();
            if (system.IsPoweredOn()) {
                system.Shutdown();
            }
            UWP::RestoreGlobalConfig();
        });
        title_running = false;
        emulation_paused = false;
        in_game_menu_visible = false;
        in_game_menu_popup_open = false;
        current_title_path.clear();
        current_title_id = 0;
        pending_incompatible_state_load.reset();
        movie_dialog.SetMode(false, false);
        preparing_loaded_title = false;
        auto_start_requested_for_title = false;
        remote_launch_stream_requested_for_title = false;
        pending_broadcast_authorization_token = 0;
        pending_broadcast_stream_mask = 0;
        loading_screen.Hide();
        discord_presence.SetPresence("Not playing", "Idle");
        InputCommon::CemuhookUDP::SetRemoteFrontendState({});
        if (refresh_library) {
            game_list.Refresh();
        }
    };

    // Loading runs on the emulation worker; the CoreWindow thread keeps drawing and pumping events.
    const auto attempt_boot = [&](const std::string& path, bool remote_frontend_launch) {
        if (startup_storage_notice_pending) {
            deferred_startup_activation_path = path;
            return;
        }
        if (path.empty()) {
            boot_error = "No application path was selected.";
            return;
        }
        if (pending_boot.has_value()) {
            boot_error = "Another application is already loading.";
            return;
        }

        const u64 program_id_hint = game_list.FindProgramIdByPath(path).value_or(0);
        game_list.CancelScan();
        // A title may be replaced without passing through stop_title(); persist its play time here.
        game_list.StopPlayTimeTracking();
        if (title_running) {
            request_broadcast_stop();
        }
        emulation_thread.PauseAndWait();
        emulation_thread.RunExclusive(stop_broadcasts_exclusive);
        pending_broadcast_authorization_token = 0;
        pending_broadcast_stream_mask = 0;
        auto_start_requested_for_title = false;
        remote_launch_stream_requested_for_title = remote_frontend_launch;
        title_running = false;
        InputCommon::CemuhookUDP::SetRemoteFrontendState({});
        emulation_paused = false;
        in_game_menu_visible = false;
        in_game_menu_popup_open = false;
        pending_incompatible_state_load.reset();
        preparing_loaded_title = false;
        const auto record_request = std::exchange(queued_movie_record, std::nullopt);
        const auto playback_request = std::exchange(queued_movie_playback, std::nullopt);

        auto task = std::make_shared<std::packaged_task<BootOutcome()>>(
            [&, path, program_id_hint, record_request, playback_request] {
                BootOutcome result;
                try {
                    // Movie clock overrides must be installed before System::Load constructs the
                    // timing state.
                    if (system.IsPoweredOn()) {
                        system.Shutdown();
                        UWP::RestoreGlobalConfig();
                    }
                    if (playback_request) {
                        system.Movie().PrepareForPlayback(*playback_request);
                    } else if (record_request) {
                        system.Movie().PrepareForRecording();
                    }

                    if (!BootTitle(system, emu_window, path, program_id_hint, result.error)) {
                        if (system.IsPoweredOn()) {
                            system.Shutdown();
                        }
                        UWP::RestoreGlobalConfig();
                        return result;
                    }

                    if (playback_request) {
                        system.Movie().StartPlayback(*playback_request);
                        if (system.Movie().GetPlayMode() != Core::Movie::PlayMode::Playing) {
                            throw std::runtime_error("the movie file could not be opened");
                        }
                        result.movie_playing = true;
                    } else if (record_request) {
                        system.Movie().StartRecording(record_request->path, record_request->author);
                        result.movie_recording = true;
                    }
                    system.GetAppLoader().ReadProgramId(result.title_id);
                    result.success = true;
                } catch (const std::exception& exception) {
                    result.error = fmt::format("Unhandled {} while loading: {}",
                                               typeid(exception).name(), exception.what());
                    if (system.IsPoweredOn()) {
                        system.Shutdown();
                    }
                    UWP::RestoreGlobalConfig();
                } catch (...) {
                    result.error = "Unhandled non-standard exception while loading the title.";
                    if (system.IsPoweredOn()) {
                        system.Shutdown();
                    }
                    UWP::RestoreGlobalConfig();
                }
                return result;
            });
        auto outcome = task->get_future();
        if (!emulation_thread.PostExclusive([task] { (*task)(); })) {
            queued_movie_record = record_request;
            queued_movie_playback = playback_request;
            remote_launch_stream_requested_for_title = false;
            boot_error = "The emulation worker is not available.";
            return;
        }

        const std::string title = std::filesystem::path(path).stem().string();
        loading_screen.Show(title.empty() ? "Application" : title);
        loading_screen.SetProgress("Loading application...", 0.0f);
        pending_boot.emplace(PendingBoot{path, std::move(outcome)});
    };

    const auto finish_pending_boot = [&] {
        if (!pending_boot ||
            pending_boot->outcome.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            return;
        }

        const std::string path = pending_boot->path;
        BootOutcome result;
        try {
            result = pending_boot->outcome.get();
        } catch (const std::exception& exception) {
            result.error = fmt::format("Loading task failed: {}", exception.what());
        } catch (...) {
            result.error = "Loading task failed with an unknown exception.";
        }
        pending_boot.reset();

        if (!result.success) {
            boot_error = result.error.empty() ? "Failed to load the title." : result.error;
            title_running = false;
            remote_launch_stream_requested_for_title = false;
            InputCommon::CemuhookUDP::SetRemoteFrontendState({});
            loading_screen.Hide();
            game_list.Refresh();
            return;
        }

        title_running = true;
        current_title_path = path;
        current_title_id = result.title_id;
        if (!app_in_background) {
            game_list.StartPlayTimeTracking(current_title_id);
        }
        movie_dialog.SetMode(result.movie_recording, result.movie_playing);
        UWP::AddRecentFile(path);
        main_menu.SetRecentFiles(UWP::values.recent_files);
        UWP::SaveConfig();
        // Restart the library scan that was cancelled before loading.
        game_list.Refresh();
        const std::string presence_title = std::filesystem::path(path).stem().string();
        discord_presence.SetPresence(
            presence_title.empty() ? "Unknown application" : presence_title, "Playing on Xbox");
        InputCommon::CemuhookUDP::SetRemoteFrontendState(
            {1, presence_title.empty() ? "Unknown application" : presence_title, "Nintendo 3DS",
             "Azahar"});
        in_game_menu_visible = false;
        in_game_menu_popup_open = false;
        if (app_in_background) {
            if (UWP::values.pause_when_in_background.GetValue()) {
                emulation_paused = true;
                auto_paused_in_background = true;
            }
            if (UWP::values.mute_when_in_background.GetValue() && !Settings::values.audio_muted) {
                Settings::values.audio_muted = true;
                auto_muted_in_background = true;
            }
        }
        preparing_loaded_title = emulation_thread.RequestPrepareLoadedTitle();
        if (!preparing_loaded_title) {
            loading_screen.Hide();
            emulation_thread.Resume();
        } else {
            loading_screen.SetProgress("Preparing graphics resources...", 0.0f);
        }
    };

    const auto update_load_progress = [&] {
        while (const auto progress = emulation_thread.TakeLoadProgress()) {
            const char* stage = "Preparing...";
            switch (progress->stage) {
            case VideoCore::LoadCallbackStage::Preload:
                stage = "Preloading custom textures...";
                break;
            case VideoCore::LoadCallbackStage::Decompile:
                stage = "Decompiling cached shaders...";
                break;
            case VideoCore::LoadCallbackStage::Build:
                stage = "Building cached shaders...";
                break;
            case VideoCore::LoadCallbackStage::Complete:
                stage = "Launching...";
                break;
            case VideoCore::LoadCallbackStage::Prepare:
            default:
                stage = progress->switching_resources ? "Switching title resources..."
                                                      : "Preparing graphics resources...";
                break;
            }
            const float fraction = progress->total == 0
                                       ? 0.0f
                                       : std::clamp(static_cast<float>(progress->value) /
                                                        static_cast<float>(progress->total),
                                                    0.0f, 1.0f);
            loading_screen.SetProgress(stage, fraction);
        }
        if (preparing_loaded_title && emulation_thread.IsReady()) {
            preparing_loaded_title = false;
            loading_screen.Hide();
        }
    };

    const auto process_remote_stream_commands = [&] {
        std::vector<InputCommon::CemuhookUDP::RemoteStreamCommand> commands;
        {
            std::lock_guard lock{remote_stream_mutex};
            commands = std::exchange(remote_stream_commands, {});
        }
        for (const auto& command : commands) {
            const auto report_error = [&](u32 code) {
                InputCommon::CemuhookUDP::QueueRemoteStreamStatus(2, code, command.stream_type,
                                                                  command.screen_id, command.url, 0,
                                                                  0, command.audio_enabled);
            };
            if (command.stream_type != 3 && command.stream_type != 4) {
                report_error(1);
                continue;
            }
            if (command.start && !command.url.starts_with("udp://")) {
                report_error(1);
                continue;
            }
            if (command.start &&
                (((command.width == 0) != (command.height == 0)) ||
                 (command.width != 0 && (command.width < 2 || command.height < 2)) ||
                 command.width > 4096 || command.height > 4096 || command.fps == 0 ||
                 command.fps > 60)) {
                report_error(1);
                continue;
            }
            if (!title_running || !system.IsPoweredOn()) {
                report_error(2);
                continue;
            }
            if ((command.auxiliary && command.screen_id != 1) ||
                (!command.auxiliary && command.screen_id != 0)) {
                report_error(3);
                continue;
            }

            const auto authorization_now = std::chrono::steady_clock::now();
            if (pending_broadcast_authorization_token != 0 &&
                authorization_now > pending_broadcast_authorization_deadline) {
                pending_broadcast_authorization_token = 0;
                pending_broadcast_stream_mask = 0;
            }
            const u8 stream_bit = command.auxiliary ? 1U << 1 : 1U << 0;
            const bool locally_authorized =
                command.start && command.authorization_token != 0 &&
                command.authorization_token == pending_broadcast_authorization_token &&
                (pending_broadcast_stream_mask & stream_bit) != 0;
            if (!UWP::values.allow_broadcasting.GetValue() ||
                (command.authorization_token != 0 && !locally_authorized) ||
                (!UWP::values.allow_third_party_broadcast_control.GetValue() &&
                 !locally_authorized)) {
                report_error(6);
                continue;
            }
            if (command.start) {
                // FFmpeg is loaded lazily by the dumping dialog, which remote commands bypass.
                if (!DynamicLibrary::FFmpeg::LoadFFmpeg()) {
                    report_error(5);
                    continue;
                }
            }

            const bool resume_after_command = emulation_thread.IsRunning();
            emulation_thread.Pause();
            emulation_thread.Synchronize();
            bool success = false;
            u16 stream_width = 0;
            u16 stream_height = 0;
            std::optional<u8> stopped_opposite_screen_id;
            try {
                success = emulation_thread.RunExclusive([&] {
                    auto& renderer = system.GPU().Renderer();
                    if (!command.start) {
                        if (command.auxiliary) {
                            if (!remote_aux_stream) {
                                return true;
                            }
                            renderer.StopAuxVideoDumping();
                            system.RegisterAuxVideoDumper(nullptr);
                            if (remote_aux_stream->IsDumping()) {
                                remote_aux_stream->StopDumping();
                            }
                            remote_aux_stream.reset();
                        } else {
                            if (!remote_main_stream) {
                                return true;
                            }
                            if (remote_main_stream->IsDumping()) {
                                remote_main_stream->StopDumping();
                            }
                            system.RegisterVideoDumper(nullptr);
                            remote_main_stream.reset();
                        }
                        return true;
                    }

                    const auto current_dumper = system.GetVideoDumper();
                    if (current_dumper && current_dumper->IsDumping() &&
                        current_dumper != remote_main_stream) {
                        return false;
                    }
                    // Only one remotely selected screen is streamed at a time.
                    if (!command.auxiliary && remote_aux_stream) {
                        renderer.StopAuxVideoDumping();
                        system.RegisterAuxVideoDumper(nullptr);
                        if (remote_aux_stream->IsDumping()) {
                            remote_aux_stream->StopDumping();
                        }
                        remote_aux_stream.reset();
                        stopped_opposite_screen_id = 1;
                    } else if (command.auxiliary && remote_main_stream) {
                        if (remote_main_stream->IsDumping()) {
                            remote_main_stream->StopDumping();
                        }
                        system.RegisterVideoDumper(nullptr);
                        remote_main_stream.reset();
                        stopped_opposite_screen_id = 0;
                    }
                    if (!command.auxiliary) {
                        if (remote_main_stream) {
                            if (remote_main_stream->IsDumping()) {
                                remote_main_stream->StopDumping();
                            }
                            system.RegisterVideoDumper(nullptr);
                            remote_main_stream.reset();
                        }
                    } else if (remote_aux_stream) {
                        renderer.StopAuxVideoDumping();
                        system.RegisterAuxVideoDumper(nullptr);
                        if (remote_aux_stream->IsDumping()) {
                            remote_aux_stream->StopDumping();
                        }
                        remote_aux_stream.reset();
                    }

                    const Layout::FramebufferLayout source_layout = [&] {
                        if (!command.auxiliary) {
                            return renderer.GetRenderWindow().GetFramebufferLayout();
                        }
                        const u32 scale = renderer.GetResolutionScaleFactor();
                        u32 width = Core::kScreenBottomWidth * scale;
                        u32 height = Core::kScreenBottomHeight * scale;
                        const bool upright = Settings::values.upright_screen.GetValue();
                        if (upright) {
                            std::swap(width, height);
                        }
                        // The auxiliary feed is always the physical lower screen, regardless of
                        // the window layout.
                        return Layout::SingleFrameLayout(width, height, true, upright);
                    }();
                    const Layout::FramebufferLayout layout =
                        FitBroadcastLayout(source_layout, command.width, command.height);
                    if (layout.width == 0 || layout.height == 0) {
                        return false;
                    }
                    stream_width = static_cast<u16>(
                        std::min<u32>(layout.width, std::numeric_limits<u16>::max()));
                    stream_height = static_cast<u16>(
                        std::min<u32>(layout.height, std::numeric_limits<u16>::max()));

                    const auto saved_format = Settings::values.output_format;
                    const auto saved_format_options = Settings::values.format_options;
                    const auto saved_video_encoder = Settings::values.video_encoder;
                    const auto saved_video_options = Settings::values.video_encoder_options;
                    const auto saved_video_bitrate = Settings::values.video_bitrate;
                    const auto saved_audio_encoder = Settings::values.audio_encoder;
                    const auto saved_audio_options = Settings::values.audio_encoder_options;
                    const auto saved_audio_bitrate = Settings::values.audio_bitrate;
                    SCOPE_EXIT({
                        Settings::values.output_format = saved_format;
                        Settings::values.format_options = saved_format_options;
                        Settings::values.video_encoder = saved_video_encoder;
                        Settings::values.video_encoder_options = saved_video_options;
                        Settings::values.video_bitrate = saved_video_bitrate;
                        Settings::values.audio_encoder = saved_audio_encoder;
                        Settings::values.audio_encoder_options = saved_audio_options;
                        Settings::values.audio_bitrate = saved_audio_bitrate;
                    });

                    Settings::values.output_format = "mpegts";
                    Settings::values.format_options = "flush_packets:1,mpegts_flags:resend_headers";
                    Settings::values.video_encoder = "h264_mf";
                    Settings::values.video_encoder_options.clear();
                    Settings::values.video_bitrate =
                        std::max<u64>(500'000, static_cast<u64>(command.bitrate_kbps) * 1000);
                    Settings::values.audio_encoder = "aac";
                    Settings::values.audio_encoder_options.clear();
                    Settings::values.audio_bitrate = 128'000;

                    VideoDumper::FFmpegBackendConfig backend_config;
                    backend_config.manage_renderer_capture = !command.auxiliary;
                    backend_config.audio_enabled = command.audio_enabled;
                    backend_config.target_fps = command.fps;
                    backend_config.low_latency = true;
                    auto backend =
                        std::make_shared<VideoDumper::FFmpegBackend>(renderer, backend_config);
                    const bool started = backend->StartDumping(command.url, layout);
                    if (!started) {
                        return false;
                    }
                    if (command.auxiliary) {
                        remote_aux_stream = backend;
                        renderer.StartAuxVideoDumping(backend);
                        system.RegisterAuxVideoDumper(backend);
                    } else {
                        remote_main_stream = backend;
                        system.RegisterVideoDumper(backend);
                    }
                    return true;
                });
            } catch (const std::exception& exception) {
                LOG_ERROR(Frontend, "DSU stream command failed: {}", exception.what());
                success = false;
            }
            if (resume_after_command) {
                emulation_thread.Resume();
            }
            if (stopped_opposite_screen_id) {
                InputCommon::CemuhookUDP::QueueRemoteStreamStatus(0, 0, command.stream_type,
                                                                  *stopped_opposite_screen_id, "");
            }
            if (!success) {
                report_error(4);
            } else {
                if (locally_authorized) {
                    pending_broadcast_stream_mask &= ~stream_bit;
                    if (pending_broadcast_stream_mask == 0) {
                        pending_broadcast_authorization_token = 0;
                    }
                }
                InputCommon::CemuhookUDP::QueueRemoteStreamStatus(
                    command.start ? 1 : 0, 0, command.stream_type, command.screen_id,
                    command.start ? command.url : std::string{}, stream_width, stream_height,
                    command.audio_enabled);
            }
        }
    };

    const auto start_cia_install = [&](const std::string& path) {
        if (file_job.active) {
            boot_error = "Another file operation is still running.";
            return;
        }
        file_job.Start(
            "Installing " + std::filesystem::path(path).filename().string(),
            [path](FileJob& job) -> std::string {
                const auto status =
                    Service::AM::InstallCIA(path, [&job](std::size_t written, std::size_t total) {
                        job.progress = written;
                        job.total = total;
                    });
                switch (status) {
                case Service::AM::InstallStatus::Success:
                    return "Installed " + path;
                case Service::AM::InstallStatus::ErrorEncrypted:
                    return "Failed to install: the CIA is encrypted. Set up system files, or "
                           "decrypt it on a console first.";
                case Service::AM::InstallStatus::ErrorFailedToOpenFile:
                case Service::AM::InstallStatus::ErrorFileNotFound:
                    return "Failed to install: could not open " + path;
                default:
                    return "Failed to install: the CIA file is invalid.";
                }
            });
    };

    const auto handle_activated_file = [&](const std::string& path) {
        if (path.empty()) {
            return;
        }
        std::string extension = std::filesystem::path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".cia" || extension == ".zcia") {
            start_cia_install(path);
        } else {
            attempt_boot(path, false);
        }
    };

    const auto start_system_setup = [&] {
        const bool is_o3ds = sysfiles_mode == 0;
        const std::string address = sysfiles_addr.data();
        if (title_running) {
            stop_title(false);
        }
        Core::UninstallSystemFiles(is_o3ds ? Core::SystemTitleSet::Old3ds
                                           : Core::SystemTitleSet::New3ds);
        attempt_boot(std::string{is_o3ds ? "articinio://" : "articinin://"} + address, false);
    };

    // Controller hotkeys are polled every frame with edge detection so they work during gameplay.
    constexpr std::size_t num_hotkeys = static_cast<std::size_t>(UWP::ControllerHotkey::Count);
    std::array<std::unique_ptr<Input::ButtonDevice>, num_hotkeys> hotkey_devices;
    std::array<bool, num_hotkeys> hotkey_was_pressed{};
    std::array<std::unique_ptr<Input::ButtonDevice>, 2> menu_chord_devices;
    bool menu_chord_was_pressed = false;
    const auto reload_hotkeys = [&] {
        for (std::size_t i = 0; i < num_hotkeys; ++i) {
            const std::string& params = UWP::values.controller_hotkeys[i];
            hotkey_devices[i] =
                params.empty() ? nullptr : Input::CreateDevice<Input::ButtonDevice>(params);
            hotkey_was_pressed[i] = false;
        }
        for (std::size_t i = 0; i < menu_chord_devices.size(); ++i) {
            const std::string& params = UWP::values.in_game_menu_chord[i];
            menu_chord_devices[i] =
                params.empty() ? nullptr : Input::CreateDevice<Input::ButtonDevice>(params);
        }
        menu_chord_was_pressed = false;
    };
    reload_hotkeys();
    // Read the DSU L3 button independently so local and remote controllers share the hold action.
    auto dsu_l3_device = Input::CreateDevice<Input::ButtonDevice>(
        InputCommon::CemuhookUDP::GenerateButtonParam(InputCommon::CemuhookUDP::Button::L3)
            .Serialize());

    // Configuration is saved periodically because UWP may suspend the process without shutdown.
    auto last_autosave = std::chrono::steady_clock::now();
    bool broadcasting_was_allowed = UWP::values.allow_broadcasting.GetValue();
    std::optional<std::chrono::steady_clock::time_point> l3_hold_started;
    bool l3_hold_fired = false;

    // Cold file activation is delivered before the main loop; handle it once everything is ready.
    if (startup_storage_notice_pending) {
        deferred_startup_activation_path = startup_activation_path;
    } else {
        handle_activated_file(startup_activation_path);
    }

    // The CoreWindow thread presents at a steady 60 Hz; emulation has its own limiter.
    constexpr auto frontend_frame_interval = std::chrono::microseconds{16'667};
    auto next_frontend_frame = std::chrono::steady_clock::now();

    while (!emu_window.ShouldQuit()) {
        emu_window.PollEvents(&imgui);
        const bool broadcasting_allowed = UWP::values.allow_broadcasting.GetValue();
        remote_stream_capabilities_enabled.store(
            broadcasting_allowed && UWP::values.allow_third_party_broadcast_control.GetValue(),
            std::memory_order_relaxed);
        if (!broadcasting_allowed && broadcasting_was_allowed) {
            stop_active_broadcasts();
            auto_start_requested_for_title = false;
        }
        // Remote stream control requests one start event for each launched title.
        const bool remote_stream_control_enabled =
            broadcasting_allowed && UWP::values.allow_third_party_broadcast_control.GetValue();
        const bool should_auto_start_broadcast = UWP::values.auto_start_broadcasts.GetValue() ||
                                                 remote_stream_control_enabled ||
                                                 remote_launch_stream_requested_for_title;
        if (!should_auto_start_broadcast) {
            auto_start_requested_for_title = false;
        }
        broadcasting_was_allowed = broadcasting_allowed;

        // The renderer consumes the framebuffer layout on the emulation thread, so stop at an
        // exclusive boundary before publishing a new one.
        if (!preparing_loaded_title && emu_window.TakeLayoutUpdateRequest()) {
            emulation_thread.Pause();
            emulation_thread.Synchronize();
            emu_window.UpdateLayout();
        }
        std::optional<std::string> dsu_launch;
        {
            std::lock_guard lock{remote_launch_mutex};
            dsu_launch = std::exchange(remote_launch_path, std::nullopt);
        }
        if (dsu_launch) {
            attempt_boot(*dsu_launch, true);
        }
        std::optional<u64> dsu_select;
        {
            std::lock_guard lock{remote_select_mutex};
            dsu_select = std::exchange(remote_select_token, std::nullopt);
        }
        if (dsu_select && game_list.SelectRemote(*dsu_select)) {
            // Echo an unchanged selection too, so the frontend sees its request acknowledged.
            last_remote_selection.reset();
        }
        // Repeat periodically so a frontend that connects later still learns the focus.
        const auto frontend_selection = game_list.GetRemoteSelection();
        const auto selection_now = std::chrono::steady_clock::now();
        if (frontend_selection &&
            (frontend_selection != last_remote_selection ||
             selection_now - last_remote_selection_sent > std::chrono::seconds{2})) {
            last_remote_selection = frontend_selection;
            last_remote_selection_sent = selection_now;
            InputCommon::CemuhookUDP::QueueRemoteAppSelection(frontend_selection->revision,
                                                              frontend_selection->token);
        }
        process_remote_stream_commands();
        finish_pending_boot();
        update_load_progress();
        bool reaped_stopped_broadcast = false;
        if (remote_main_stream && !remote_main_stream->IsDumping()) {
            remote_main_stream.reset();
            InputCommon::CemuhookUDP::QueueRemoteStreamStatus(0, 0, 4, 0, "");
            reaped_stopped_broadcast = true;
        }
        if (remote_aux_stream && !remote_aux_stream->IsDumping()) {
            remote_aux_stream.reset();
            InputCommon::CemuhookUDP::QueueRemoteStreamStatus(0, 0, 4, 1, "");
            reaped_stopped_broadcast = true;
        }
        if (reaped_stopped_broadcast) {
            pending_broadcast_authorization_token = 0;
            pending_broadcast_stream_mask = 0;
            auto_start_requested_for_title = false;
        }
        if (pending_broadcast_authorization_token != 0 &&
            std::chrono::steady_clock::now() > pending_broadcast_authorization_deadline) {
            pending_broadcast_authorization_token = 0;
            pending_broadcast_stream_mask = 0;
            if (should_auto_start_broadcast) {
                auto_start_requested_for_title = false;
            }
        }
        if (title_running && !preparing_loaded_title && !pending_boot && broadcasting_allowed &&
            should_auto_start_broadcast && !auto_start_requested_for_title) {
            const u8 missing_streams = static_cast<u8>((~active_broadcast_mask()) & 0x03);
            auto_start_requested_for_title =
                missing_streams == 0 || request_broadcast_start(missing_streams);
        }

        while (const auto worker_error = emulation_thread.TakeError()) {
            const bool guest_shutdown =
                worker_error->status == Core::System::ResultStatus::ShutdownRequested;
            if (worker_error->status == Core::System::ResultStatus::ErrorSavestate) {
                boot_error = worker_error->details.empty()
                                 ? "The save-state operation failed."
                                 : "Save-state operation failed: " + worker_error->details;
                continue;
            }
            if (!guest_shutdown) {
                LOG_ERROR(Frontend, "Emulation stopped with status {}: {}",
                          static_cast<int>(worker_error->status), worker_error->details);
            }
            stop_title();
            if (!guest_shutdown) {
                boot_error = worker_error->details.empty()
                                 ? "Emulation stopped unexpectedly."
                                 : "Emulation stopped: " + worker_error->details;
            }
        }

        if (movie_playback_completed.exchange(false)) {
            emulation_paused = true;
            emulation_thread.Pause();
            movie_dialog.SetMode(false, false);
            boot_error = "Movie playback completed.";
        }

        if (const auto dropped_file = emu_window.TakeDroppedFile(); dropped_file.has_value()) {
            handle_activated_file(*dropped_file);
        }

        if (emu_window.TakeEnteredBackground()) {
            app_in_background = true;
            if (title_running) {
                // The app may be terminated after suspension; flush play time now and do not
                // count it.
                game_list.StopPlayTimeTracking();
                if (UWP::values.pause_when_in_background.GetValue() && !emulation_paused) {
                    emulation_paused = true;
                    auto_paused_in_background = true;
                    emulation_thread.Pause();
                }
                if (UWP::values.mute_when_in_background.GetValue() &&
                    !Settings::values.audio_muted) {
                    Settings::values.audio_muted = true;
                    auto_muted_in_background = true;
                }
            }
        }
        if (emu_window.TakeEnteredForeground()) {
            app_in_background = false;
            if (title_running) {
                game_list.StartPlayTimeTracking(current_title_id);
            }
            if (auto_paused_in_background) {
                emulation_paused = false;
                auto_paused_in_background = false;
            }
            if (auto_muted_in_background) {
                Settings::values.audio_muted = false;
                auto_muted_in_background = false;
            }
        }

        // The controller is reported shortly after launch, so retry until the seed lands.
        if (!pending_boot) {
            SeedDefaultInputBindings();
        }

        const auto frontend_now = std::chrono::steady_clock::now();
        if (frontend_now < next_frontend_frame) {
            std::this_thread::sleep_until(next_frontend_frame);
        }
        next_frontend_frame = std::chrono::steady_clock::now() + frontend_frame_interval;

        // Only the frontmost UI layer may own controller input, even where a window is transparent.
        const bool frontend_dialog_open =
            configure.IsOpen() || movie_dialog.IsDialogOpen() || dumping_dialog.IsOpen() ||
            cheats_dialog.IsOpen() || about_dialog.IsOpen() || file_browser.IsOpen() ||
            multiplayer.IsWindowOpen() || discord_presence.IsOpen() ||
            software_keyboard->IsOpen() || mii_selector->IsOpen() ||
            UWP::GetVirtualKeyboard().IsOpen() || loading_screen.IsVisible() || file_job.active ||
            startup_storage_notice_pending || pending_incompatible_state_load.has_value() ||
            !boot_error.empty() || exit_confirmation_pending || artic_popup_pending ||
            artic_popup_open || sysfiles_popup_pending || sysfiles_popup_open ||
            sysfiles_reinstall_confirmation_pending;
        const bool gameplay_pointer_active = title_running && !preparing_loaded_title &&
                                             !pending_boot && !in_game_menu_visible &&
                                             !frontend_dialog_open;
        emu_window.UpdateVirtualTouchCursor(gameplay_pointer_active);
        imgui.SetRemoteMouseForFrontend(!gameplay_pointer_active);
        imgui.SetGameplayInputActive(gameplay_pointer_active);

        // OpenGLState's cache belongs to the worker context; set state on the primary directly.
        const auto& presentation_layout = emu_window.GetFramebufferLayout();
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glViewport(0, 0, static_cast<GLsizei>(presentation_layout.width),
                   static_cast<GLsizei>(presentation_layout.height));

        // A guest-requested reset recreates the GPU on the worker while this thread keeps
        // presenting; the core is unpowered for the whole reload.
        if (title_running && !preparing_loaded_title && !pending_boot && system.IsPoweredOn()) {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            system.GPU().Renderer().TryPresent(0);
        } else {
            glClearColor(0.06f, 0.07f, 0.09f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }

        imgui.BeginFrame(presentation_layout.width, presentation_layout.height);

        if (gameplay_pointer_active) {
            float mouse_x = 0.0f;
            float mouse_y = 0.0f;
            if (emu_window.GetMouseCursor(mouse_x, mouse_y)) {
                imgui.DrawMouseCursor(mouse_x, mouse_y);
            }
            const auto cursor = emu_window.GetCursorInfo();
            const auto& cursor_layout = emu_window.GetFramebufferLayout();
            if (cursor.visible && cursor_layout.width != 0 && cursor_layout.height != 0) {
                const ImGuiIO& io = ImGui::GetIO();
                const float scale_x = io.DisplaySize.x / cursor_layout.width;
                const float scale_y = io.DisplaySize.y / cursor_layout.height;
                const ImVec2 centre{
                    (cursor_layout.bottom_screen.left + cursor.projected_x) * scale_x,
                    (cursor_layout.bottom_screen.top + cursor.projected_y) * scale_y};
                const float radius =
                    std::max(8.0f, cursor_layout.bottom_screen.GetHeight() * scale_y / 30.0f);
                ImDrawList* draw = ImGui::GetForegroundDrawList();
                const ImU32 outline = IM_COL32(0, 0, 0, 220);
                const ImU32 foreground = IM_COL32(255, 255, 255, 245);
                draw->AddLine({centre.x - radius, centre.y}, {centre.x + radius, centre.y}, outline,
                              5.0f);
                draw->AddLine({centre.x, centre.y - radius}, {centre.x, centre.y + radius}, outline,
                              5.0f);
                draw->AddLine({centre.x - radius, centre.y}, {centre.x + radius, centre.y},
                              foreground, 2.0f);
                draw->AddLine({centre.x, centre.y - radius}, {centre.x, centre.y + radius},
                              foreground, 2.0f);
            }
        }

        // Start reaches the emulated 3DS during gameplay; only the configured chord opens the menu.
        const bool frontend_menu_requested = imgui.TakeMenuFocusRequest();
        const bool popup_owns_input =
            ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        const bool menu_input_blocked =
            frontend_dialog_open || game_list.HasOpenOverlay() || popup_owns_input;
        const bool menu_chord_valid =
            menu_chord_devices[0] && menu_chord_devices[1] &&
            UWP::values.in_game_menu_chord[0] != UWP::values.in_game_menu_chord[1];
        const bool menu_chord_pressed = title_running && menu_chord_valid &&
                                        menu_chord_devices[0]->GetStatus() &&
                                        menu_chord_devices[1]->GetStatus();
        const bool menu_chord_activated = menu_chord_pressed && !menu_chord_was_pressed;
        menu_chord_was_pressed = menu_chord_pressed;
        // Escape opens the in-game menu from the keyboard; within menus it acts as Back.
        const auto& key_presses = UWP::KeyboardInput::KeyPresses();
        const bool escape_pressed =
            gameplay_pointer_active &&
            std::any_of(key_presses.begin(), key_presses.end(), [](const auto& press) {
                return press.key == SDLK_ESCAPE && press.modifiers == 0;
            });
        const bool menu_toggle_requested =
            !menu_input_blocked &&
            ((frontend_menu_requested && !title_running) || menu_chord_activated || escape_pressed);

        UWP::MenuContext menu_context;
        menu_context.input_enabled = !frontend_dialog_open;
        menu_context.emulation_running = title_running;
        menu_context.emulation_paused = emulation_paused;
        menu_context.show_status_bar = UWP::values.show_status_overlay.GetValue();
        menu_context.fullscreen = true;
        menu_context.single_window_mode = true;
        menu_context.in_room = multiplayer.IsInRoom();
        menu_context.movie_recording =
            movie_dialog.IsRecording() || queued_movie_record.has_value();
        menu_context.movie_playing = movie_dialog.IsPlaying() || queued_movie_playback.has_value();
        menu_context.movie_read_only = movie_dialog.IsReadOnly();
        const auto video_dumper = system.GetVideoDumper();
        menu_context.video_dumping = title_running && video_dumper && video_dumper->IsDumping();
        menu_context.room_hosted = multiplayer.IsHosting();
        menu_context.allow_broadcasting = UWP::values.allow_broadcasting.GetValue();
        const u8 broadcast_mask = active_broadcast_mask() | pending_broadcast_stream_mask;
        menu_context.main_broadcast_active = (broadcast_mask & (1U << 0)) != 0;
        menu_context.auxiliary_broadcast_active = (broadcast_mask & (1U << 1)) != 0;

        UWP::MenuAction menu_action = UWP::MenuAction::None;
        bool menu_action_from_quick_menu = false;
        const bool game_list_visible = !title_running && !pending_boot;
        menu_context.game_list_visible = game_list_visible;
        menu_context.library_favorites_first = UWP::values.game_list_favorite_first.GetValue();
        menu_context.library_favorites_only = UWP::values.game_list_favorites_only.GetValue();
        menu_context.cartridge_inserted = !UWP::values.inserted_cartridge.GetValue().empty();
        menu_context.screenscraper_ready = UWP::values.screenscraper_enabled.GetValue() &&
                                           UWP::ScreenScraper::HasDeveloperCredentials() &&
                                           UWP::ScreenScraper::HasUserCredentials();
        menu_context.library_busy = game_list.IsScanning() || game_list.IsScraping();
        if (game_list_visible) {
            if (menu_toggle_requested) {
                imgui.FocusMainMenuBar();
            }
            game_list.SetMenuCallback([&] { menu_action = main_menu.DrawContents(menu_context); });
        } else {
            if (menu_toggle_requested && !preparing_loaded_title) {
                in_game_menu_visible = !in_game_menu_visible;
                in_game_menu_popup_open = false;
                if (in_game_menu_visible) {
                    refresh_save_state_slots();
                    main_menu.FocusEmulation();
                }
                // Retry item focus across the overlay's first two submitted frames.
                menu_focus_frames = in_game_menu_visible ? 2 : 0;
            }
            if (in_game_menu_visible) {
                menu_context.save_state_slots = available_save_state_slots;
                if (menu_focus_frames > 0) {
                    main_menu.FocusEmulation();
                }
                const bool close_menu_requested =
                    !menu_toggle_requested && !in_game_menu_popup_open &&
                    ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
                menu_action = main_menu.Draw(menu_context);
                in_game_menu_popup_open = main_menu.HasOpenTopLevelPopup();
                if (menu_focus_frames > 0) {
                    --menu_focus_frames;
                }
                menu_action_from_quick_menu = menu_action != UWP::MenuAction::None;
                if (close_menu_requested || menu_action != UWP::MenuAction::None) {
                    in_game_menu_visible = false;
                    in_game_menu_popup_open = false;
                    menu_focus_frames = 0;
                }
            }
        }

        // The game list hosts the menu bar, so it is drawn before the action switch.
        if (game_list_visible) {
            const std::string launch = game_list.Draw();
            if (const auto request = game_list.TakeConfigureRequest(); request.has_value()) {
                configure.OpenPerGame(request->program_id, request->path, false);
            }
            if (!launch.empty() && !startup_storage_notice_pending) {
                menu_return_action = UWP::MenuAction::None;
                attempt_boot(launch, false);
            }
        }

        // Holding L3 swaps the prominent 3DS screen without changing the selected layout.
        constexpr auto l3_screen_swap_hold_time = std::chrono::milliseconds{750};
        const bool l3_pressed =
            gameplay_pointer_active &&
            (imgui.IsLeftStickPressed() || (dsu_l3_device && dsu_l3_device->GetStatus()));
        if (!l3_pressed) {
            l3_hold_started.reset();
            l3_hold_fired = false;
        } else if (!l3_hold_started) {
            l3_hold_started = std::chrono::steady_clock::now();
        } else if (!l3_hold_fired && std::chrono::steady_clock::now() - *l3_hold_started >=
                                         l3_screen_swap_hold_time) {
            menu_action = UWP::MenuAction::ScreenLayoutSwapScreens;
            l3_hold_fired = true;
        }

        const auto run_hotkey = [&](UWP::ControllerHotkey hotkey) {
            switch (hotkey) {
            case UWP::ControllerHotkey::TogglePause:
                if (title_running) {
                    menu_action = UWP::MenuAction::Pause;
                }
                break;
            case UWP::ControllerHotkey::StopEmulation:
                if (title_running) {
                    menu_action = UWP::MenuAction::Stop;
                }
                break;
            case UWP::ControllerHotkey::QuickSave:
                if (title_running) {
                    menu_action = UWP::MenuAction::QuickSave;
                }
                break;
            case UWP::ControllerHotkey::QuickLoad:
                if (title_running) {
                    menu_action = UWP::MenuAction::QuickLoad;
                }
                break;
            case UWP::ControllerHotkey::CaptureScreenshot:
                if (title_running) {
                    menu_action = UWP::MenuAction::CaptureScreenshot;
                }
                break;
            case UWP::ControllerHotkey::SwapScreens:
                // The L3 hold above already invokes this command; a direct L3 hotkey would
                // fire it twice.
                if (!l3_pressed) {
                    menu_action = UWP::MenuAction::ScreenLayoutSwapScreens;
                }
                break;
            case UWP::ControllerHotkey::RestartEmulation:
                if (title_running) {
                    menu_action = UWP::MenuAction::Restart;
                }
                break;
            case UWP::ControllerHotkey::AdvanceFrame:
                if (title_running) {
                    menu_action = UWP::MenuAction::AdvanceFrame;
                }
                break;
            case UWP::ControllerHotkey::ToggleUprightScreens:
                menu_action = UWP::MenuAction::ScreenLayoutUprightScreens;
                break;
            case UWP::ControllerHotkey::CycleLayout:
                menu_action = UWP::MenuAction::ScreenLayoutCycle;
                break;
            case UWP::ControllerHotkey::ToggleMute:
                Settings::values.audio_muted = !Settings::values.audio_muted;
                break;
            case UWP::ControllerHotkey::IncreaseSpeed:
                Settings::values.frame_limit.SetValue(
                    std::min(1000.0, Settings::values.frame_limit.GetValue() + 5.0));
                UWP::SaveConfig();
                break;
            case UWP::ControllerHotkey::DecreaseSpeed:
                Settings::values.frame_limit.SetValue(
                    std::max(5.0, Settings::values.frame_limit.GetValue() - 5.0));
                UWP::SaveConfig();
                break;
            case UWP::ControllerHotkey::ResetSpeed:
                Settings::values.frame_limit.SetValue(100.0);
                UWP::SaveConfig();
                break;
            case UWP::ControllerHotkey::Increase3D: {
                const u32 factor = Settings::values.factor_3d.GetValue();
                Settings::values.factor_3d.SetValue(std::min<u32>(255, factor + 5 - (factor % 5)));
                UWP::SaveConfig();
                break;
            }
            case UWP::ControllerHotkey::Decrease3D: {
                const u32 factor = Settings::values.factor_3d.GetValue();
                Settings::values.factor_3d.SetValue(
                    factor == 0 ? 0
                                : factor - std::min<u32>(factor, factor % 5 == 0 ? 5 : factor % 5));
                UWP::SaveConfig();
                break;
            }
            case UWP::ControllerHotkey::LoadFile:
                menu_action = UWP::MenuAction::LoadFile;
                break;
            case UWP::ControllerHotkey::Exit:
                menu_action = UWP::MenuAction::Exit;
                break;
            case UWP::ControllerHotkey::LoadAmiibo:
                if (title_running) {
                    menu_action = UWP::MenuAction::LoadAmiibo;
                }
                break;
            case UWP::ControllerHotkey::RemoveAmiibo:
                if (title_running) {
                    menu_action = UWP::MenuAction::RemoveAmiibo;
                }
                break;
            case UWP::ControllerHotkey::SaveToOldestSlot:
                if (title_running) {
                    menu_action = UWP::MenuAction::SaveToOldestSlot;
                }
                break;
            case UWP::ControllerHotkey::LoadFromNewestSlot:
                if (title_running) {
                    menu_action = UWP::MenuAction::LoadFromNewestSlot;
                }
                break;
            case UWP::ControllerHotkey::ToggleFilterBar:
                if (game_list_visible) {
                    menu_action = UWP::MenuAction::SearchGameList;
                }
                break;
            case UWP::ControllerHotkey::ToggleStatusBar:
                menu_action = UWP::MenuAction::ShowStatusBar;
                break;
            case UWP::ControllerHotkey::ToggleCustomTextures:
                Settings::values.custom_textures = !Settings::values.custom_textures;
                break;
            case UWP::ControllerHotkey::BrowsePublicRooms:
                menu_action = UWP::MenuAction::ViewLobby;
                break;
            case UWP::ControllerHotkey::CreateRoom:
                menu_action = UWP::MenuAction::StartRoom;
                break;
            case UWP::ControllerHotkey::LeaveRoom:
                if (multiplayer.IsInRoom()) {
                    menu_action = UWP::MenuAction::LeaveRoom;
                }
                break;
            case UWP::ControllerHotkey::ShowCurrentRoom:
                if (multiplayer.IsInRoom()) {
                    menu_action = UWP::MenuAction::ShowRoom;
                }
                break;
            default:
                break;
            }
        };

        // Suppressed while the configure dialog is open so a rebind press cannot fire a hotkey.
        if (menu_action == UWP::MenuAction::None && !configure.IsOpen() &&
            !preparing_loaded_title && !pending_boot) {
            bool hotkey_fired = false;
            for (std::size_t i = 0; i < num_hotkeys; ++i) {
                if (!hotkey_devices[i]) {
                    continue;
                }
                const bool pressed = hotkey_devices[i]->GetStatus();
                const bool fired = pressed && !hotkey_was_pressed[i];
                hotkey_was_pressed[i] = pressed;
                if (!fired) {
                    continue;
                }
                run_hotkey(static_cast<UWP::ControllerHotkey>(i));
                hotkey_fired = true;
                break; // At most one hotkey fires per frame.
            }
            // Text fields keep their own editing shortcuts.
            if (!hotkey_fired && !ImGui::GetIO().WantTextInput) {
                const auto& chords = UWP::values.keyboard_hotkeys;
                for (const auto& press : key_presses) {
                    const auto match =
                        std::find_if(chords.begin(), chords.end(), [&press](const auto& chord) {
                            return !chord.empty() && UWP::KeyboardInput::ChordMatches(chord, press);
                        });
                    if (match != chords.end()) {
                        run_hotkey(static_cast<UWP::ControllerHotkey>(match - chords.begin()));
                        break;
                    }
                }
            }
        }

        const auto remember_menu_page = [&](UWP::MenuAction action) {
            menu_return_action = action;
            menu_return_to_quick_menu = menu_action_from_quick_menu;
        };

        switch (menu_action) {
        case UWP::MenuAction::Configure:
            if (title_running) {
                emulation_thread.PauseAndWait();
            }
            configure.Open();
            remember_menu_page(menu_action);
            break;
        case UWP::MenuAction::ConfigureCurrentGame:
            if (title_running) {
                emulation_thread.PauseAndWait();
                configure.OpenPerGame(current_title_id, current_title_path, true);
                remember_menu_page(menu_action);
            }
            break;
        case UWP::MenuAction::Cheats:
            cheats_dialog.Open();
            remember_menu_page(menu_action);
            break;
        case UWP::MenuAction::RecordMovie:
            movie_dialog.OpenRecord();
            remember_menu_page(menu_action);
            break;
        case UWP::MenuAction::PlayMovie:
            movie_dialog.OpenPlay();
            remember_menu_page(menu_action);
            break;
        case UWP::MenuAction::CloseMovie:
            if (queued_movie_record || queued_movie_playback) {
                queued_movie_record.reset();
                queued_movie_playback.reset();
                boot_error = "Pending movie operation cancelled.";
            } else {
                emulation_thread.RunExclusive([&] { movie_dialog.Close(system); });
            }
            break;
        case UWP::MenuAction::SaveMovie:
            emulation_thread.RunExclusive([&] { movie_dialog.SaveWithoutClosing(system); });
            break;
        case UWP::MenuAction::MovieReadOnlyMode:
            emulation_thread.RunExclusive([&] { movie_dialog.ToggleReadOnly(system); });
            break;
        case UWP::MenuAction::DumpVideo:
            dumping_dialog.Open();
            remember_menu_page(menu_action);
            break;
        case UWP::MenuAction::StartBroadcasts: {
            const u8 missing_streams = static_cast<u8>((~active_broadcast_mask()) & 0x03);
            if (missing_streams != 0 && !request_broadcast_start(missing_streams)) {
                boot_error =
                    "Broadcasts require a running game and Allow Broadcasting to be enabled.";
            }
            break;
        }
        case UWP::MenuAction::StopBroadcasts:
            stop_active_broadcasts();
            break;
        case UWP::MenuAction::About:
            about_dialog.Open();
            remember_menu_page(menu_action);
            break;
        case UWP::MenuAction::ViewLobby:
            multiplayer.OpenLobby();
            remember_menu_page(menu_action);
            break;
        case UWP::MenuAction::StartRoom:
            multiplayer.OpenHostRoom();
            remember_menu_page(menu_action);
            break;
        case UWP::MenuAction::ConnectToRoom:
            multiplayer.OpenDirectConnect();
            remember_menu_page(menu_action);
            break;
        case UWP::MenuAction::ShowRoom:
            multiplayer.OpenClientRoom();
            remember_menu_page(menu_action);
            break;
        case UWP::MenuAction::LeaveRoom:
            multiplayer.LeaveRoom();
            break;
        case UWP::MenuAction::Exit:
            if (title_running && UWP::values.confirm_before_closing.GetValue()) {
                exit_confirmation_pending = true;
            } else {
                emu_window.RequestQuit();
            }
            break;
        case UWP::MenuAction::SaveState:
        case UWP::MenuAction::LoadState:
        case UWP::MenuAction::QuickSave:
        case UWP::MenuAction::QuickLoad:
        case UWP::MenuAction::SaveToOldestSlot:
        case UWP::MenuAction::LoadFromNewestSlot: {
            u32 slot = 0;
            const bool is_load = menu_action == UWP::MenuAction::LoadState ||
                                 menu_action == UWP::MenuAction::QuickLoad ||
                                 menu_action == UWP::MenuAction::LoadFromNewestSlot;
            bool state_request_queued = false;
            try {
                emulation_thread.RunExclusive([&] {
                    u64 title = 0;
                    system.GetAppLoader().ReadProgramId(title);
                    const u64 movie_id = system.Movie().GetCurrentMovieID();
                    const auto states = Core::ListSaveStates(title, movie_id);
                    std::array<u64, Core::SaveStateSlotCount> times{};
                    for (const auto& info : states) {
                        if (info.slot < Core::SaveStateSlotCount) {
                            times[info.slot] = info.time;
                        }
                    }

                    if (menu_action == UWP::MenuAction::SaveState ||
                        menu_action == UWP::MenuAction::LoadState) {
                        slot = main_menu.SelectedStateSlot();
                    } else if (menu_action == UWP::MenuAction::SaveToOldestSlot) {
                        slot = 1;
                        for (u32 i = 1; i < Core::SaveStateSlotCount; ++i) {
                            if (times[i] == 0) {
                                slot = i;
                                break;
                            }
                            if (times[i] < times[slot]) {
                                slot = i;
                            }
                        }
                    } else if (menu_action == UWP::MenuAction::LoadFromNewestSlot) {
                        u64 newest_time = 0;
                        // Slot 0 is Quick Save and is excluded from the numbered slots, as on
                        // desktop.
                        for (u32 i = 1; i < Core::SaveStateSlotCount; ++i) {
                            if (times[i] > newest_time) {
                                newest_time = times[i];
                                slot = i;
                            }
                        }
                        if (newest_time == 0) {
                            throw std::runtime_error(
                                "There are no numbered save states for this title yet");
                        }
                    }

                    if (is_load && times[slot] == 0) {
                        throw std::runtime_error("the selected save-state slot is empty");
                    }
                    if (is_load) {
                        const auto selected = std::find_if(
                            states.begin(), states.end(),
                            [slot](const Core::SaveStateInfo& info) { return info.slot == slot; });
                        if (selected != states.end() &&
                            selected->status ==
                                Core::SaveStateInfo::ValidationStatus::RevisionMismatch) {
                            pending_incompatible_state_load =
                                PendingIncompatibleStateLoad{slot, selected->build_name};
                            return;
                        }
                    }
                    const auto signal =
                        is_load ? Core::System::Signal::Load : Core::System::Signal::Save;
                    if (!system.SendSignal(signal, slot)) {
                        throw std::runtime_error(
                            "another core operation is already waiting to run");
                    }
                    state_request_queued = true;
                });
                // Save/load must pass through RunLoop so the core can wait for outstanding
                // filesystem work. Request exactly one frame when the user left it paused.
                if (state_request_queued) {
                    if (emulation_paused) {
                        emulation_thread.RequestFrameAdvance();
                    } else {
                        emulation_thread.Resume();
                    }
                }
            } catch (const std::exception& e) {
                boot_error = fmt::format("{} state in slot {} failed: {}",
                                         is_load ? "Loading" : "Saving", slot, e.what());
            }
            break;
        }
        case UWP::MenuAction::Pause:
            emulation_paused = !emulation_paused;
            if (emulation_paused) {
                emulation_thread.Pause();
            }
            break;
        case UWP::MenuAction::Stop:
            stop_title();
            break;
        case UWP::MenuAction::Restart: {
            const std::string path = current_title_path;
            if (!path.empty()) {
                emulation_paused = false;
                attempt_boot(path, false);
            }
            break;
        }
        case UWP::MenuAction::CaptureScreenshot:
            emulation_thread.RunExclusive([&] {
                CaptureScreenshot(system, UWP::values.screenshot_path.GetValue(),
                                  current_title_path, boot_error);
            });
            if (emulation_paused) {
                emulation_thread.RequestFrameAdvance();
            }
            break;
        case UWP::MenuAction::LoadFile:
            file_browser.Open("Load File", {".cci", ".zcci", ".3ds", ".cxi", ".zcxi", ".app",
                                            ".3dsx", ".z3dsx", ".elf", ".axf"});
            pending_file_action = FileAction::LoadFile;
            remember_menu_page(menu_action);
            break;
        case UWP::MenuAction::LoadRecentFile:
            attempt_boot(main_menu.SelectedRecentFile(), false);
            break;
        case UWP::MenuAction::ClearRecentFiles:
            UWP::values.recent_files.clear();
            main_menu.SetRecentFiles({});
            UWP::SaveConfig();
            break;
        case UWP::MenuAction::EjectCartridge:
            UWP::values.inserted_cartridge.SetValue("");
            UWP::SaveConfig();
            break;
        case UWP::MenuAction::InstallCIA:
            if (file_job.active) {
                boot_error = "Another file operation is still running.";
            } else {
                file_browser.Open("Install CIA", {".cia", ".zcia"});
                pending_file_action = FileAction::InstallCIA;
                remember_menu_page(menu_action);
            }
            break;
        case UWP::MenuAction::LoadAmiibo:
            file_browser.Open("Load Amiibo", {".bin"});
            pending_file_action = FileAction::LoadAmiibo;
            remember_menu_page(menu_action);
            break;
        case UWP::MenuAction::RemoveAmiibo:
            emulation_thread.RunExclusive([&] {
                if (auto nfc = system.ServiceManager().GetService<Service::NFC::Module::Interface>(
                        "nfc:u")) {
                    std::scoped_lock lock{system.Kernel().GetHLELock()};
                    nfc->RemoveAmiibo();
                }
            });
            break;
        case UWP::MenuAction::BootHomeMenuJPN:
        case UWP::MenuAction::BootHomeMenuUSA:
        case UWP::MenuAction::BootHomeMenuEUR:
        case UWP::MenuAction::BootHomeMenuAUS:
        case UWP::MenuAction::BootHomeMenuCHN:
        case UWP::MenuAction::BootHomeMenuKOR:
        case UWP::MenuAction::BootHomeMenuTWN: {
            const u32 region =
                static_cast<u32>(menu_action) - static_cast<u32>(UWP::MenuAction::BootHomeMenuJPN);
            const std::string path = Core::GetHomeMenuNcchPath(region);
            if (path.empty() || !FileUtil::Exists(path)) {
                boot_error = "The Home Menu for this region is not installed. Install it with "
                             "File > Set Up System Files first.";
            } else {
                attempt_boot(path, false);
            }
            break;
        }
        case UWP::MenuAction::ConnectArtic:
            artic_popup_pending = true;
            artic_popup_open = true;
            remember_menu_page(menu_action);
            break;
        case UWP::MenuAction::SetupSystemFiles:
            sysfiles_installed = Core::AreSystemTitlesInstalled();
            // Preselect whichever mode is still missing, like the Qt dialog.
            sysfiles_mode = (sysfiles_installed.first && !sysfiles_installed.second) ? 1 : 0;
            sysfiles_popup_pending = true;
            sysfiles_popup_open = true;
            remember_menu_page(menu_action);
            break;
        case UWP::MenuAction::CompressROMFile:
            if (file_job.active) {
                boot_error = "Another file operation is still running.";
            } else {
                file_browser.Open("Compress ROM File", {".cia", ".cci", ".3ds", ".cxi", ".3dsx"});
                pending_file_action = FileAction::Compress;
                remember_menu_page(menu_action);
            }
            break;
        case UWP::MenuAction::DecompressROMFile:
            if (file_job.active) {
                boot_error = "Another file operation is still running.";
            } else {
                file_browser.Open("Decompress ROM File", {".zcia", ".zcci", ".z3dsx", ".zcxi"});
                pending_file_action = FileAction::Decompress;
                remember_menu_page(menu_action);
            }
            break;
        case UWP::MenuAction::AdvanceFrame:
            if (title_running) {
                emulation_paused = true;
                emulation_thread.RequestFrameAdvance();
            }
            break;
        case UWP::MenuAction::DebugStep:
            if (title_running) {
                emulation_paused = true;
                emulation_thread.RequestSingleStep();
            }
            break;
        case UWP::MenuAction::DebugPause:
            emulation_paused = true;
            emulation_thread.Pause();
            break;
        case UWP::MenuAction::DebugResume:
            emulation_paused = false;
            break;
        case UWP::MenuAction::DebugUnscheduleAll:
            emulation_thread.RunExclusive(
                [&] { system.DebugUnscheduleAllThreadsFromFrontend(true); });
            break;
        case UWP::MenuAction::DebugScheduleAll:
            emulation_thread.RunExclusive(
                [&] { system.DebugUnscheduleAllThreadsFromFrontend(false); });
            break;
        case UWP::MenuAction::ShowStatusBar:
            UWP::values.show_status_overlay.SetValue(!UWP::values.show_status_overlay.GetValue());
            UWP::SaveConfig();
            break;
        case UWP::MenuAction::ReportCompatibility:
            LaunchExternalUri(
                L"https://github.com/azahar-emu/compatibility-list/blob/master/CONTRIBUTING.md",
                boot_error);
            break;
        case UWP::MenuAction::OpenUserFolder:
            boot_error = FileUtil::GetUserPath(FileUtil::UserPath::UserDir);
            break;
        case UWP::MenuAction::OpenNANDFolder:
            boot_error = FileUtil::GetUserPath(FileUtil::UserPath::NANDDir);
            break;
        case UWP::MenuAction::OpenSDMCFolder:
            boot_error = FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir);
            break;
        case UWP::MenuAction::OpenLogFolder:
            boot_error = FileUtil::GetUserPath(FileUtil::UserPath::LogDir);
            break;
        case UWP::MenuAction::ScrapeGameLibrary:
            game_list.ScrapeLibrary();
            break;

        case UWP::MenuAction::ScreenLayoutDefault:
        case UWP::MenuAction::ScreenLayoutSingleScreen:
        case UWP::MenuAction::ScreenLayoutLargeScreen:
        case UWP::MenuAction::ScreenLayoutSideBySide:
        case UWP::MenuAction::ScreenLayoutSeparateWindows:
        case UWP::MenuAction::ScreenLayoutHybridScreen:
        case UWP::MenuAction::ScreenLayoutCustomLayout:
        case UWP::MenuAction::ScreenLayoutSwapScreens:
        case UWP::MenuAction::ScreenLayoutUprightScreens:
        case UWP::MenuAction::ScreenLayoutCycle:
        case UWP::MenuAction::SmallScreenTopRight:
        case UWP::MenuAction::SmallScreenMiddleRight:
        case UWP::MenuAction::SmallScreenBottomRight:
        case UWP::MenuAction::SmallScreenTopLeft:
        case UWP::MenuAction::SmallScreenMiddleLeft:
        case UWP::MenuAction::SmallScreenBottomLeft:
        case UWP::MenuAction::SmallScreenAbove:
        case UWP::MenuAction::SmallScreenBelow: {
            // Layout changes are applied once the worker reaches an exclusive boundary.
            emulation_thread.Pause();
            emulation_thread.Synchronize();
            switch (menu_action) {
            case UWP::MenuAction::ScreenLayoutDefault:
                Settings::values.layout_option.SetValue(Settings::LayoutOption::Default);
                break;
            case UWP::MenuAction::ScreenLayoutSingleScreen:
                Settings::values.layout_option.SetValue(Settings::LayoutOption::SingleScreen);
                break;
            case UWP::MenuAction::ScreenLayoutLargeScreen:
                Settings::values.layout_option.SetValue(Settings::LayoutOption::LargeScreen);
                break;
            case UWP::MenuAction::ScreenLayoutSideBySide:
                Settings::values.layout_option.SetValue(Settings::LayoutOption::SideScreen);
                break;
            case UWP::MenuAction::ScreenLayoutSeparateWindows:
                Settings::values.layout_option.SetValue(Settings::LayoutOption::Default);
                break;
            case UWP::MenuAction::ScreenLayoutHybridScreen:
                Settings::values.layout_option.SetValue(Settings::LayoutOption::HybridScreen);
                break;
            case UWP::MenuAction::ScreenLayoutCustomLayout:
                Settings::values.layout_option.SetValue(Settings::LayoutOption::CustomLayout);
                break;
            case UWP::MenuAction::ScreenLayoutSwapScreens:
                Settings::values.swap_screen.SetValue(!Settings::values.swap_screen.GetValue());
                break;
            case UWP::MenuAction::ScreenLayoutUprightScreens:
                Settings::values.upright_screen.SetValue(
                    !Settings::values.upright_screen.GetValue());
                break;
            case UWP::MenuAction::ScreenLayoutCycle: {
                auto layouts = Settings::values.layouts_to_cycle.GetValue();
                std::erase(layouts, Settings::LayoutOption::SeparateWindows);
                if (layouts.empty()) {
                    layouts.push_back(Settings::LayoutOption::Default);
                }
                const auto current = Settings::values.layout_option.GetValue();
                const auto found = std::find(layouts.begin(), layouts.end(), current);
                const auto next = found == layouts.end() || std::next(found) == layouts.end()
                                      ? layouts.begin()
                                      : std::next(found);
                Settings::values.layout_option.SetValue(*next);
                break;
            }
            case UWP::MenuAction::SmallScreenTopRight:
                Settings::values.small_screen_position.SetValue(
                    Settings::SmallScreenPosition::TopRight);
                break;
            case UWP::MenuAction::SmallScreenMiddleRight:
                Settings::values.small_screen_position.SetValue(
                    Settings::SmallScreenPosition::MiddleRight);
                break;
            case UWP::MenuAction::SmallScreenBottomRight:
                Settings::values.small_screen_position.SetValue(
                    Settings::SmallScreenPosition::BottomRight);
                break;
            case UWP::MenuAction::SmallScreenTopLeft:
                Settings::values.small_screen_position.SetValue(
                    Settings::SmallScreenPosition::TopLeft);
                break;
            case UWP::MenuAction::SmallScreenMiddleLeft:
                Settings::values.small_screen_position.SetValue(
                    Settings::SmallScreenPosition::MiddleLeft);
                break;
            case UWP::MenuAction::SmallScreenBottomLeft:
                Settings::values.small_screen_position.SetValue(
                    Settings::SmallScreenPosition::BottomLeft);
                break;
            case UWP::MenuAction::SmallScreenAbove:
                Settings::values.small_screen_position.SetValue(
                    Settings::SmallScreenPosition::AboveLarge);
                break;
            case UWP::MenuAction::SmallScreenBelow:
                Settings::values.small_screen_position.SetValue(
                    Settings::SmallScreenPosition::BelowLarge);
                break;
            default:
                break;
            }
            UWP::NormalizeDisplaySettings();
            emu_window.UpdateLayout();
            if (title_running) {
                emulation_thread.RunExclusive([&] {
                    UWP::NormalizeDisplaySettings();
                    system.ApplySettings();
                });
            }
            UWP::SaveConfig();
            break;
        }

        case UWP::MenuAction::FAQ:
            LaunchExternalUri(L"https://azahar-emu.org/pages/faq/", boot_error);
            break;
        case UWP::MenuAction::DiscordXboxEmulationHub:
            LaunchExternalUri(L"https://discord.gg/WCmxvvxHqu", boot_error);
            break;
        case UWP::MenuAction::DiscordRevivesCommunity:
            LaunchExternalUri(L"https://discord.com/invite/f82Tr22vRZ", boot_error);
            break;
        case UWP::MenuAction::AppStore:
            LaunchExternalUri(L"https://emulationrevival.github.io/", boot_error);
            break;
        case UWP::MenuAction::Fullscreen:
        case UWP::MenuAction::SingleWindowMode:
            // A console app is always full-screen and single-window.
            break;
        case UWP::MenuAction::SearchGameList:
            game_list.OpenSearch();
            remember_menu_page(menu_action);
            break;
        case UWP::MenuAction::ToggleFavoritesFirst:
            UWP::values.game_list_favorite_first.SetValue(
                !UWP::values.game_list_favorite_first.GetValue());
            game_list.ApplyLibraryPreferences();
            UWP::SaveConfig();
            break;
        case UWP::MenuAction::ToggleFavoritesOnly:
            UWP::values.game_list_favorites_only.SetValue(
                !UWP::values.game_list_favorites_only.GetValue());
            game_list.ApplyLibraryPreferences();
            UWP::SaveConfig();
            break;
        case UWP::MenuAction::RefreshGameList:
            game_list.Refresh();
            break;
        default:
            break;
        }

        const bool core_dialog_open = configure.IsOpen() || movie_dialog.IsDialogOpen() ||
                                      dumping_dialog.IsOpen() || cheats_dialog.IsOpen() ||
                                      multiplayer.IsWindowOpen() || software_keyboard->IsOpen() ||
                                      mii_selector->IsOpen();
        if (configure.IsOpen()) {
            // The global System page reads CFG through Core::System, so reach an exclusive
            // boundary first.
            if (title_running) {
                emulation_thread.PauseAndWait();
            } else {
                emulation_thread.Synchronize();
            }
        } else if (title_running && core_dialog_open) {
            emulation_thread.PauseAndWait();
        }

        multiplayer.Draw();
        if (!pending_boot) {
            movie_dialog.Draw(system);
        }
        dumping_dialog.Draw(system);
        cheats_dialog.Draw(system);
        about_dialog.Draw();
        switch (about_dialog.TakeSupportLinkRequest()) {
        case UWP::AboutDialog::SupportLink::XboxEmulationHub:
            LaunchExternalUri(L"https://discord.gg/WCmxvvxHqu", boot_error);
            break;
        case UWP::AboutDialog::SupportLink::RevivesCommunity:
            LaunchExternalUri(L"https://discord.com/invite/f82Tr22vRZ", boot_error);
            break;
        case UWP::AboutDialog::SupportLink::None:
            break;
        }
        loading_screen.Draw();

        if (auto output_path = dumping_dialog.TakeStartRequest()) {
            menu_return_action = UWP::MenuAction::None;
            bool started = false;
            std::string error;
            if (remote_main_stream || remote_aux_stream) {
                error = "Stop DSU network streaming before starting a local video dump.";
            } else if (!title_running || !system.IsPoweredOn()) {
                error = "No application is running.";
            } else {
                emulation_thread.PauseAndWait();
                try {
                    started = emulation_thread.RunExclusive([&] {
                        auto& renderer = system.GPU().Renderer();
                        const auto layout = Layout::FrameLayoutFromResolutionScale(
                            renderer.GetResolutionScaleFactor());
                        auto dumper = std::make_shared<VideoDumper::FFmpegBackend>(renderer);
                        if (!dumper->StartDumping(*output_path, layout)) {
                            return false;
                        }
                        system.RegisterVideoDumper(std::move(dumper));
                        return true;
                    });
                    if (!started) {
                        error = "Could not start video dumping. Check the selected encoders and "
                                "azahar_log.txt for the FFmpeg error.";
                    }
                } catch (const std::exception& exception) {
                    error = std::string{"Could not start video dumping: "} + exception.what();
                }
            }
            dumping_dialog.CompleteStart(started, std::move(error));
            UWP::SaveConfig();
        }

        if (dumping_dialog.TakeStopRequest()) {
            menu_return_action = UWP::MenuAction::None;
            std::string error;
            emulation_thread.PauseAndWait();
            try {
                emulation_thread.RunExclusive([&] {
                    const auto dumper = system.GetVideoDumper();
                    if (dumper && dumper->IsDumping()) {
                        dumper->StopDumping();
                    }
                    system.RegisterVideoDumper(nullptr);
                    if (remote_main_stream && dumper == remote_main_stream) {
                        remote_main_stream.reset();
                        InputCommon::CemuhookUDP::QueueRemoteStreamStatus(0, 0, 4, 0, "");
                    }
                });
            } catch (const std::exception& exception) {
                error = std::string{"Could not finalize video dumping: "} + exception.what();
            }
            dumping_dialog.CompleteStop(std::move(error));
        }

        if (auto request = movie_dialog.TakeRecordRequest()) {
            menu_return_action = UWP::MenuAction::None;
            std::filesystem::path movie_path = request->path;
            if (movie_path.extension().empty()) {
                movie_path.replace_extension(".ctm");
            }
            if (movie_path.is_relative()) {
                movie_path =
                    std::filesystem::path{FileUtil::GetUserPath(FileUtil::UserPath::UserDir)} /
                    "movies" / movie_path;
            }
            const std::string movie_directory = movie_path.parent_path().generic_string() + '/';
            if (!FileUtil::IsDirectory(movie_directory) &&
                !FileUtil::CreateFullPath(movie_directory)) {
                boot_error = "Could not create movie directory " + movie_directory;
            } else {
                request->path = movie_path.string();
                queued_movie_playback.reset();
                queued_movie_record = std::move(*request);
                if (title_running && !current_title_path.empty()) {
                    attempt_boot(current_title_path, false);
                } else {
                    boot_error =
                        "Movie recording is ready and will begin when an application is launched.";
                }
            }
        }
        if (auto request = movie_dialog.TakePlaybackRequest()) {
            menu_return_action = UWP::MenuAction::None;
            try {
                const auto metadata = emulation_thread.RunExclusive(
                    [&] { return system.Movie().GetMovieMetadata(*request); });
                std::optional<std::string> game_path;
                if (title_running && metadata.program_id == current_title_id) {
                    game_path = current_title_path;
                } else {
                    game_path = game_list.FindPathByProgramId(metadata.program_id);
                }
                if (!game_path || game_path->empty()) {
                    boot_error = fmt::format(
                        "The application for this movie ({:016X}) is not in the Applications "
                        "list.",
                        metadata.program_id);
                } else {
                    queued_movie_record.reset();
                    queued_movie_playback = std::move(*request);
                    attempt_boot(*game_path, false);
                }
            } catch (const std::exception& exception) {
                boot_error = std::string{"Could not start movie playback: "} + exception.what();
            }
        }

        file_browser.Draw();
        if (const std::string picked = file_browser.TakeResult(); !picked.empty()) {
            menu_return_action = UWP::MenuAction::None;
            const FileAction action = std::exchange(pending_file_action, FileAction::None);
            switch (action) {
            case FileAction::LoadFile:
                attempt_boot(picked, false);
                break;
            case FileAction::InstallCIA:
                start_cia_install(picked);
                break;
            case FileAction::LoadAmiibo: {
                emulation_thread.RunExclusive([&] {
                    auto nfc = system.ServiceManager().GetService<Service::NFC::Module::Interface>(
                        "nfc:u");
                    if (!nfc) {
                        boot_error = "The NFC service is not running.";
                        return;
                    }
                    std::scoped_lock lock{system.Kernel().GetHLELock()};
                    if (nfc->IsTagActive()) {
                        boot_error = "An amiibo tag is already in use.";
                    } else if (!nfc->IsSearchingForAmiibos()) {
                        boot_error = "The application is not currently looking for amiibos.";
                    } else if (!nfc->LoadAmiibo(picked)) {
                        boot_error = "Unable to open amiibo file " + picked;
                    }
                });
                break;
            }
            case FileAction::Compress:
            case FileAction::Decompress: {
                const bool compress = action == FileAction::Compress;
                const auto info = Loader::GetCompressFileInfo(picked, compress);
                if (!info.has_value()) {
                    // Encrypted files are rejected: compressing high-entropy data saves nothing.
                    boot_error = compress ? "This file cannot be compressed (it may be "
                                            "encrypted or an unsupported format)."
                                          : "This file is not a supported compressed ROM.";
                    break;
                }
                const std::string extension = compress
                                                  ? info->first.recommended_compressed_extension
                                                  : info->first.recommended_uncompressed_extension;
                const std::string out_path =
                    std::filesystem::path(picked).replace_extension(extension).string();
                if (FileUtil::Exists(out_path)) {
                    boot_error =
                        "The destination already exists and was not overwritten: " + out_path;
                    break;
                }
                file_job.Start(
                    (compress ? "Compressing to " : "Decompressing to ") + out_path,
                    [picked, out_path, compress, info](FileJob& job) -> std::string {
                        const auto progress = [&job](std::size_t written, std::size_t total) {
                            job.progress = written;
                            job.total = total;
                        };
                        const bool ok =
                            compress ? FileUtil::CompressZ3DSFile(
                                           picked, out_path, info->first.underlying_magic,
                                           info->second, progress, info->first.default_metadata)
                                     : FileUtil::DeCompressZ3DSFile(picked, out_path, progress);
                        if (!ok) {
                            FileUtil::Delete(out_path);
                            return std::string{compress ? "Compression" : "Decompression"} +
                                   " failed.";
                        }
                        return "Wrote " + out_path;
                    });
                break;
            }
            default:
                break;
            }
        }

        if (artic_popup_pending) {
            ImGui::OpenPopup("Connect to Artic Base");
            artic_popup_pending = false;
        }
        if (ImGui::BeginPopupModal("Connect to Artic Base", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextUnformatted("Enter the address shown by the Artic Base app on the console:");
            UWP::GetVirtualKeyboard().InputText("Address", artic_addr.data(), artic_addr.size());
            const bool have_addr = artic_addr[0] != '\0';
            if (ImGui::Button("Connect", ImVec2(160.0f, 0.0f)) && have_addr) {
                artic_popup_open = false;
                menu_return_action = UWP::MenuAction::None;
                ImGui::CloseCurrentPopup();
                attempt_boot(std::string{"articbase://"} + artic_addr.data(), false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(160.0f, 0.0f)) ||
                (!ImGui::IsAnyItemActive() &&
                 ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
                artic_popup_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (sysfiles_popup_pending) {
            ImGui::OpenPopup("Set Up System Files");
            sysfiles_popup_pending = false;
        }
        if (ImGui::BeginPopupModal("Set Up System Files", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextWrapped(
                "Azahar needs console-unique data and firmware files from a real 3DS. Run the "
                "Azahar Artic Setup Tool on the console and enter its address here. Do not "
                "share your user or NAND folders after setup; the data is unique to your "
                "console.");
            ImGui::Spacing();
            UWP::GetVirtualKeyboard().InputText("Setup tool address", sysfiles_addr.data(),
                                                sysfiles_addr.size());
            ImGui::Spacing();
            ImGui::RadioButton(sysfiles_installed.first ? "Old 3DS setup (already set up)"
                                                        : "Old 3DS setup",
                               &sysfiles_mode, 0);
            if (!sysfiles_installed.first) {
                ImGui::BeginDisabled();
                ImGui::RadioButton("New 3DS setup (requires Old 3DS setup first)", &sysfiles_mode,
                                   1);
                ImGui::EndDisabled();
            } else {
                ImGui::RadioButton(sysfiles_installed.second ? "New 3DS setup (already set up)"
                                                             : "New 3DS setup",
                                   &sysfiles_mode, 1);
            }
            ImGui::Spacing();
            const bool have_addr = sysfiles_addr[0] != '\0';
            if (ImGui::Button("Start Setup", ImVec2(200.0f, 0.0f)) && have_addr) {
                ImGui::CloseCurrentPopup();
                const bool is_o3ds = sysfiles_mode == 0;
                if ((is_o3ds && sysfiles_installed.first) ||
                    (!is_o3ds && sysfiles_installed.second)) {
                    sysfiles_reinstall_confirmation_pending = true;
                } else {
                    sysfiles_popup_open = false;
                    menu_return_action = UWP::MenuAction::None;
                    start_system_setup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(160.0f, 0.0f)) ||
                (!ImGui::IsAnyItemActive() &&
                 ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
                sysfiles_popup_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (sysfiles_reinstall_confirmation_pending) {
            ImGui::OpenPopup("Reinstall System Files?");
            sysfiles_reinstall_confirmation_pending = false;
        }
        if (ImGui::BeginPopupModal("Reinstall System Files?", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextWrapped(
                "System files for the selected mode are already installed. Reinstalling will "
                "replace them. Continue?");
            ImGui::Spacing();
            if (ImGui::Button("Reinstall", ImVec2(160.0f, 0.0f))) {
                sysfiles_popup_open = false;
                menu_return_action = UWP::MenuAction::None;
                ImGui::CloseCurrentPopup();
                start_system_setup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(160.0f, 0.0f)) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
                sysfiles_popup_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::EndPopup();
        }

        if (file_job.active) {
            if (!ImGui::IsPopupOpen("File Operation")) {
                ImGui::OpenPopup("File Operation");
            }
            if (ImGui::BeginPopupModal("File Operation", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoSavedSettings)) {
                ImGui::TextUnformatted(file_job.label.c_str());
                const std::size_t total = file_job.total.load();
                if (total > 0) {
                    ImGui::ProgressBar(static_cast<float>(file_job.progress.load()) /
                                           static_cast<float>(total),
                                       ImVec2(400.0f, 0.0f));
                } else {
                    ImGui::TextUnformatted("Working...");
                }
                if (file_job.done) {
                    file_job.Finish();
                    boot_error = file_job.message;
                    game_list.Refresh();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // View > Show Status Bar; a console has no status bar, so this is a corner overlay.
        if (UWP::values.show_status_overlay.GetValue() && title_running) {
            const auto results = system.GetLastPerfStats();
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(16.0f, viewport->Size.y - 16.0f), ImGuiCond_Always,
                                    ImVec2(0.0f, 1.0f));
            ImGui::SetNextWindowBgAlpha(0.6f);
            if (ImGui::Begin("##status_overlay", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs)) {
                ImGui::Text("%.0f FPS   %.0f%%", results.game_fps, results.emulation_speed * 100.0);
                if (UWP::values.show_advanced_frametime_info.GetValue()) {
                    ImGui::Text("Frame %.2f ms", results.time_vblank_interval * 1000.0);
                    ImGui::Text("GPU cmd %.2f  swap %.2f ms", results.time_gpu * 1000.0,
                                results.time_swap * 1000.0);
                    ImGui::Text("IPC %.2f  SVC %.2f  other %.2f ms", results.time_hle_ipc * 1000.0,
                                results.time_hle_svc * 1000.0, results.time_remaining * 1000.0);
                }
            }
            ImGui::End();
        }

        // Drawn late so it layers over the game and any open dialog.
        software_keyboard->Draw();
        mii_selector->Draw();

        // Drawn last so it layers over whichever field opened it.
        imgui.EnsureNavFocus();

        if (pending_incompatible_state_load) {
            ImGui::OpenPopup("Load incompatible save state?");
        }
        if (ImGui::BeginPopupModal("Load incompatible save state?", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings)) {
            const std::string build = pending_incompatible_state_load &&
                                              !pending_incompatible_state_load->build_name.empty()
                                          ? pending_incompatible_state_load->build_name
                                          : "an unknown build";
            ImGui::TextWrapped(
                "This state was created by %s, not this Azahar revision. Incompatible states "
                "can crash or restore incorrectly. Load it anyway?",
                build.c_str());
            ImGui::Spacing();
            if (ImGui::Button("Load Anyway", ImVec2(160.0f, 0.0f))) {
                const u32 slot = pending_incompatible_state_load->slot;
                const bool accepted = emulation_thread.RunExclusive(
                    [&] { return system.SendSignal(Core::System::Signal::Load, slot); });
                pending_incompatible_state_load.reset();
                if (accepted) {
                    if (emulation_paused) {
                        emulation_thread.RequestFrameAdvance();
                    } else {
                        emulation_thread.Resume();
                    }
                } else {
                    boot_error = "Another core operation is already waiting to run.";
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(160.0f, 0.0f)) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
                pending_incompatible_state_load.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::EndPopup();
        }

        // Surface a missing selected drive before accepting a game launch.
        if (startup_storage_notice_pending) {
            ImGui::OpenPopup("Storage location");
        }
        if (ImGui::BeginPopupModal("Storage location", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextWrapped("%s", UWP::Storage::StartupStatus().c_str());
            ImGui::Spacing();
            ImGui::TextWrapped("Active data root: %s", UWP::Storage::ActiveRootPath().c_str());
            ImGui::Spacing();
            if (ImGui::Button("Continue", ImVec2(180.0f, 0.0f))) {
                startup_storage_notice_pending = false;
                ImGui::CloseCurrentPopup();
                const std::string activation = std::exchange(deferred_startup_activation_path, {});
                if (!activation.empty()) {
                    handle_activated_file(activation);
                }
            }
            ImGui::SetItemDefaultFocus();
            ImGui::EndPopup();
        }

        if (!boot_error.empty()) {
            ImGui::OpenPopup("Azahar");
        }
        if (ImGui::BeginPopupModal("Azahar", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextWrapped("%s", boot_error.c_str());
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(160.0f, 0.0f)) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
                boot_error.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::EndPopup();
        }

        if (exit_confirmation_pending) {
            ImGui::OpenPopup("Exit Azahar?");
            exit_confirmation_pending = false;
        }
        if (ImGui::BeginPopupModal("Exit Azahar?", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextWrapped("An application is still running. Exit and shut it down cleanly?");
            ImGui::Spacing();
            if (ImGui::Button("Exit", ImVec2(160.0f, 0.0f))) {
                emu_window.RequestQuit();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(160.0f, 0.0f)) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::EndPopup();
        }

        const bool configure_was_open = configure.IsOpen();
        const bool configure_was_per_game = configure.IsPerGame();
        configure.Draw(system);
        // A running core only picks up setting changes when told, as with the Qt dialog's Apply.
        if (configure_was_open && !configure.IsOpen()) {
            reload_hotkeys();
            UWP::SaveConfig();
            if (title_running) {
                // Publish layout changes before the worker applies renderer settings and resumes.
                emulation_thread.Pause();
                emulation_thread.Synchronize();
                UWP::NormalizeDisplaySettings();
                emu_window.UpdateLayout();
                emulation_thread.RunExclusive([&] {
                    UWP::NormalizeDisplaySettings();
                    system.ApplySettings();
                });
            } else if (!configure_was_per_game) {
                // Storage/library settings may have changed while the global dialog was open.
                game_list.CancelScan();
                game_list.Refresh();
            }
        } else if (configure.IsOpen()) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_autosave >= std::chrono::seconds(2)) {
                UWP::SaveConfig();
                last_autosave = now;
            }
        }
        if (configure.TakeDiscordPresenceRequest()) {
            discord_presence.Open();
        }

        secondary_screen.Draw(0, 0, 0);
        discord_presence.Draw();

        if (menu_return_action != UWP::MenuAction::None) {
            bool page_open = false;
            switch (menu_return_action) {
            case UWP::MenuAction::Configure:
            case UWP::MenuAction::ConfigureCurrentGame:
                page_open = configure.IsOpen();
                break;
            case UWP::MenuAction::Cheats:
                page_open = cheats_dialog.IsOpen();
                break;
            case UWP::MenuAction::RecordMovie:
            case UWP::MenuAction::PlayMovie:
                page_open = movie_dialog.IsDialogOpen();
                break;
            case UWP::MenuAction::DumpVideo:
                page_open = dumping_dialog.IsOpen();
                break;
            case UWP::MenuAction::About:
                page_open = about_dialog.IsOpen();
                break;
            case UWP::MenuAction::ViewLobby:
            case UWP::MenuAction::StartRoom:
            case UWP::MenuAction::ConnectToRoom:
            case UWP::MenuAction::ShowRoom:
                page_open = multiplayer.IsWindowOpen();
                break;
            case UWP::MenuAction::LoadFile:
            case UWP::MenuAction::InstallCIA:
            case UWP::MenuAction::LoadAmiibo:
            case UWP::MenuAction::CompressROMFile:
            case UWP::MenuAction::DecompressROMFile:
                page_open = file_browser.IsOpen();
                break;
            case UWP::MenuAction::ConnectArtic:
                page_open = artic_popup_open;
                break;
            case UWP::MenuAction::SetupSystemFiles:
                page_open = sysfiles_popup_open;
                break;
            case UWP::MenuAction::SearchGameList:
                page_open = game_list.HasOpenOverlay();
                break;
            default:
                break;
            }

            if (!page_open) {
                const UWP::MenuAction action =
                    std::exchange(menu_return_action, UWP::MenuAction::None);
                const bool return_to_quick_menu = std::exchange(menu_return_to_quick_menu, false);
                pending_file_action = FileAction::None;
                main_menu.RestoreAction(action);
                if (return_to_quick_menu && title_running) {
                    in_game_menu_visible = true;
                    in_game_menu_popup_open = false;
                    menu_focus_frames = 0;
                }
            }
        }

        const bool popup_blocks_emulation =
            ImGui::IsPopupOpen("Storage location") || ImGui::IsPopupOpen("Azahar") ||
            ImGui::IsPopupOpen("Exit Azahar?") ||
            ImGui::IsPopupOpen("Load incompatible save state?") ||
            ImGui::IsPopupOpen("Connect to Artic Base") ||
            ImGui::IsPopupOpen("Set Up System Files") ||
            ImGui::IsPopupOpen("Reinstall System Files?") || ImGui::IsPopupOpen("File Operation");
        const bool frontend_blocks_emulation =
            pending_boot.has_value() || preparing_loaded_title || emulation_paused ||
            in_game_menu_visible || configure.IsOpen() || movie_dialog.IsDialogOpen() ||
            dumping_dialog.IsOpen() || cheats_dialog.IsOpen() || about_dialog.IsOpen() ||
            file_browser.IsOpen() || multiplayer.IsWindowOpen() || discord_presence.IsOpen() ||
            software_keyboard->IsOpen() || mii_selector->IsOpen() ||
            UWP::GetVirtualKeyboard().IsOpen() || file_job.active || popup_blocks_emulation;
        if (title_running && !frontend_blocks_emulation) {
            if (!emulation_thread.IsRunning()) {
                emulation_thread.Resume();
            }
        } else if (emulation_thread.IsRunning()) {
            emulation_thread.Pause();
        }

        imgui.EndFrame();

        emu_window.SwapBuffers();
    }

    LOG_INFO(Frontend, "Azahar UWP shutting down");
    // A file job still running holds open file handles; let it finish before teardown.
    if (file_job.active) {
        file_job.Finish();
    }
    // Tear down the core on its shared context before the primary context is destroyed.
    if (title_running) {
        request_broadcast_stop();
        InputCommon::CemuhookUDP::SetRemoteFrontendState({});
    }
    game_list.StopPlayTimeTracking();
    emulation_thread.PauseAndWait();
    emulation_thread.RunExclusive(stop_broadcasts_exclusive);
    emulation_thread.Stop();
    UWP::RestoreGlobalConfig();
    UWP::SaveConfig();
    multiplayer.Shutdown();
    return 0;
}
