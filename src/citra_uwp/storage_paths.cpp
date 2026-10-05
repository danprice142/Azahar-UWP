// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "citra_uwp/storage_paths.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>

#include "common/file_util.h"
#include "common/string_util.h"

namespace UWP::Storage {
namespace {

std::string active_root_path;
std::string startup_status;
bool startup_status_needs_attention = false;

std::string EnsureTrailingSeparator(std::string path) {
    if (!path.empty() && path.back() != '\\' && path.back() != '/') {
        path.push_back('\\');
    }
    return path;
}

std::string LocalFolderPath(bool cache) {
    const auto application_data = winrt::Windows::Storage::ApplicationData::Current();
    const auto folder =
        cache ? application_data.LocalCacheFolder() : application_data.LocalFolder();
    return EnsureTrailingSeparator(Common::UTF16ToUTF8(std::wstring{folder.Path().c_str()}));
}

bool DriveVisible(std::string_view path) {
    if (path.size() < 3 || path[1] != ':') {
        return true;
    }
    const std::wstring drive{static_cast<wchar_t>(path[0]), L':', L'\\'};
    std::error_code error;
    return std::filesystem::is_directory(std::filesystem::path{drive}, error) && !error;
}

bool ValidateWritableRoot(std::string_view root, std::string& error_out) {
    try {
        const std::filesystem::path root_path{Common::UTF8ToUTF16(root)};
        std::error_code error;
        std::filesystem::create_directories(root_path, error);
        if (error || !std::filesystem::is_directory(root_path, error) || error) {
            error_out = "The directory could not be created or opened. Check that the drive is "
                        "connected and Azahar has file-system access.";
            return false;
        }

        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path test_path =
            root_path / (L".azahar-write-test-" + std::to_wstring(nonce) + L".tmp");
        {
            std::ofstream test{test_path, std::ios::binary | std::ios::trunc};
            if (!test) {
                error_out = "The directory is visible but Azahar cannot write to it. Check Xbox "
                            "file-system permissions.";
                return false;
            }
            test << "Azahar storage write test\n";
            test.flush();
            if (!test) {
                error_out = "Writing a test file to the selected directory failed.";
                return false;
            }
        }
        std::filesystem::remove(test_path, error);
        if (error) {
            error_out = "The directory is writable, but the temporary validation file could not "
                        "be removed.";
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error_out = std::string{"Storage validation failed: "} + exception.what();
        return false;
    }
}

bool ActivateRoot(std::string_view path, std::string& error_out) {
    if (!ValidateWritableRoot(path, error_out)) {
        return false;
    }
    FileUtil::SetUserPath(EnsureTrailingSeparator(std::string{path}));
    active_root_path = FileUtil::GetUserPath(FileUtil::UserPath::UserDir);
    FileUtil::CreateFullPath(ActiveGamesPath());
    return true;
}

} // namespace

bool InitializeUserDirectory() try {
    startup_status.clear();
    startup_status_needs_attention = false;

    std::string validation_error;
    if (!ActivateRoot(LocalFolderPath(false), validation_error)) {
        const std::string fallback = LocalFolderPath(true) + "Azahar";
        if (!ActivateRoot(fallback, validation_error)) {
            startup_status = "No writable Azahar storage root is available: " + validation_error;
            return false;
        }
        startup_status = "Package LocalState is unavailable for this launch. Using " +
                         active_root_path + " temporarily. " + validation_error;
        startup_status_needs_attention = true;
    } else {
        startup_status = "Using package LocalState: " + active_root_path;
    }
    return true;
} catch (const std::exception& exception) {
    startup_status = std::string{"Storage initialization failed: "} + exception.what();
    return false;
}

const std::string& ActiveRootPath() {
    return active_root_path;
}

const std::string& StartupStatus() {
    return startup_status;
}

bool StartupStatusNeedsAttention() {
    return startup_status_needs_attention;
}

bool IsPathDriveAvailable(std::string_view path) {
    return DriveVisible(path);
}

bool EnsureDirectory(std::string_view path, std::string& error_out) {
    error_out.clear();
    if (path.empty()) {
        error_out = "No storage directory was selected.";
        return false;
    }
    if (!DriveVisible(path)) {
        error_out = "The drive is not connected.";
        return false;
    }

    const std::filesystem::path directory{Common::UTF8ToUTF16(path)};
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        error_out = "The directory could not be created: " + error.message();
        return false;
    }
    if (!std::filesystem::is_directory(directory, error) || error) {
        error_out = "The path is not an accessible directory.";
        return false;
    }
    return true;
}

bool EnsureWritableDirectory(std::string_view path, std::string& error_out) {
    return ValidateWritableRoot(path, error_out);
}

std::string ActiveGamesPath() {
    return EnsureTrailingSeparator(active_root_path + "Games");
}

} // namespace UWP::Storage
