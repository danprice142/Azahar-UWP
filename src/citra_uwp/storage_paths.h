// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <string_view>

namespace UWP::Storage {

// The Xbox development-files root (\\DEVKIT\DevelopmentFiles) maps to D: inside the console;
// Azahar keeps its folders below one app-specific directory there.
inline constexpr std::string_view DevelopmentApplicationsPath =
    "D:\\DevelopmentFiles\\Azahar\\Games\\n3ds";
inline constexpr std::string_view DevelopmentNandPath = "D:\\DevelopmentFiles\\Azahar\\Nand";
inline constexpr std::string_view DevelopmentSdmcPath = "D:\\DevelopmentFiles\\Azahar\\SDMC";
inline constexpr std::string_view DevelopmentTextureLoadPath = "D:\\DevelopmentFiles\\Azahar\\load";
inline constexpr std::string_view DevelopmentModsPath = "D:\\DevelopmentFiles\\Azahar\\load\\mods";
inline constexpr std::string_view DevelopmentTextureDumpPath = "D:\\DevelopmentFiles\\Azahar\\dump";
inline constexpr std::string_view DevelopmentCheatsPath = "D:\\DevelopmentFiles\\Azahar\\Cheats";
inline constexpr std::string_view DevelopmentScreenScraperPath =
    "D:\\DevelopmentFiles\\Azahar\\Screenscraper";

inline constexpr std::string_view ExternalApplicationsPath = "E:\\Games\\n3ds";
inline constexpr std::string_view ExternalNandPath = "E:\\Azahar\\Nand";
inline constexpr std::string_view ExternalSdmcPath = "E:\\Azahar\\SDMC";
inline constexpr std::string_view ExternalTextureLoadPath = "E:\\Azahar\\load";
inline constexpr std::string_view ExternalModsPath = "E:\\Azahar\\load\\mods";
inline constexpr std::string_view ExternalTextureDumpPath = "E:\\Azahar\\dump";
inline constexpr std::string_view ExternalCheatsPath = "E:\\Azahar\\Cheats";
inline constexpr std::string_view ExternalScreenScraperPath = "E:\\Azahar\\Screenscraper";

/// Selects package LocalState as the user directory, with LocalCache as a launch-only fallback.
bool InitializeUserDirectory();

/// The user-directory root in use.
[[nodiscard]] const std::string& ActiveRootPath();

/// Human-readable startup/fallback information for the settings page.
[[nodiscard]] const std::string& StartupStatus();

/// Whether StartupStatus describes a fallback/problem the normal startup UI must show.
[[nodiscard]] bool StartupStatusNeedsAttention();

/// Whether the drive which owns an absolute path is currently mounted.
[[nodiscard]] bool IsPathDriveAvailable(std::string_view path);

bool EnsureDirectory(std::string_view path, std::string& error_out);

/// Like EnsureDirectory, but also probes that the directory is writable.
bool EnsureWritableDirectory(std::string_view path, std::string& error_out);

[[nodiscard]] std::string ActiveGamesPath();

} // namespace UWP::Storage
