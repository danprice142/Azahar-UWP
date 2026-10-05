// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace UWP {
enum class ScreenScraperBoxArt : s32;
}

namespace UWP::ScreenScraper {

constexpr std::size_t SearchArtworkTypeCount = 9;

struct GameMetadata {
    u64 program_id = 0;
    u64 screenscraper_game_id = 0;
    std::string name;
    std::string publisher;
    std::string developer;
    std::string genre;
    std::string players;
    std::string release_date;
    std::string rating;
    std::string synopsis;
    std::string box_2d_path;
    std::string box_3d_path;
    std::string box_2d_back_path;
    std::string wheel_hd_path;
    std::string wheel_path;
    std::string support_2d_path;
    std::string mix_v1_path;
    std::string mix_v2_path;
    std::string fanart_path;
    std::string screenshot_path;
    std::string video_path;
    bool manually_edited = false;
};

struct SearchMatch {
    u64 game_id = 0;
    int system_id = 0;
    std::string name;
    std::string system;
    std::string publisher;
    std::string developer;
    std::string genre;
    std::string players;
    std::string release_date;
    std::string rating;
    std::string synopsis;
    std::string box_art_url;
    std::string large_box_art_url;
    std::array<std::string, SearchArtworkTypeCount> artwork_urls;
};

struct SearchResult {
    bool success = false;
    std::string message;
    std::vector<SearchMatch> matches;
};

struct ScrapeRequest {
    u64 program_id = 0;
    std::string title;
    std::string path;
    u64 size_bytes = 0;
    std::string preferred_region;
    u64 screenscraper_game_id = 0;
    int screenscraper_system_id = 0;
    bool force_metadata = false;
    bool persist_metadata = true;
};

struct ImageResult {
    bool success = false;
    std::string message;
    std::vector<u8> bytes;
};

struct Result {
    bool success = false;
    bool authenticated = false;
    std::string message;
    std::optional<GameMetadata> metadata;
};

/// True when the application developer credentials were injected at package-build time.
[[nodiscard]] bool HasDeveloperCredentials();

/// Stores the end-user password in the Windows Password Vault.
bool SaveUserCredentials(const std::string& username, const std::string& password,
                         std::string& error);
void ClearUserCredentials();
[[nodiscard]] bool HasUserCredentials();

/// Authenticates and reports the user's current request quota.
[[nodiscard]] Result TestLogin();

[[nodiscard]] std::optional<GameMetadata> LoadCached(u64 program_id);

/// Searches by name; a Virtual Console dump path also searches the title's original system.
[[nodiscard]] SearchResult SearchByTitle(std::string_view title,
                                         std::string_view preferred_region = {},
                                         std::string_view path = {},
                                         const std::atomic<bool>* cancelled = nullptr);

[[nodiscard]] ImageResult DownloadSearchPreview(std::string_view url, u32 max_width = 640,
                                                u32 max_height = 480,
                                                const std::atomic<bool>* cancelled = nullptr);

[[nodiscard]] const std::string& SearchArtworkUrl(const SearchMatch& match,
                                                  ScreenScraperBoxArt type);

[[nodiscard]] Result Scrape(const ScrapeRequest& request, const std::atomic<bool>* cancelled);

bool SaveMetadata(GameMetadata metadata, std::string& error);

[[nodiscard]] std::string SelectedBoxArtPath(const GameMetadata& metadata);
[[nodiscard]] std::string SelectedLargeBoxArtPath(const GameMetadata& metadata);
[[nodiscard]] std::string SelectedBackgroundPath(const GameMetadata& metadata);

} // namespace UWP::ScreenScraper
