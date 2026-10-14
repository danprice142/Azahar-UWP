// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "citra_uwp/screenscraper.h"
#include "common/common_types.h"
#include "common/play_time_manager.h"

struct ImDrawList;
struct ImGuiViewport;
struct ImVec2;

namespace Service::FS {
enum class MediaType : u32;
}

namespace UWP {

/// CPU-side detiled SMDH icon; only the presentation thread turns it into a texture.
struct GameListIcon {
    std::vector<u16> rgb565;
    u32 width = 0;
    u32 height = 0;
    u64 texture_key = 0;
};

struct GameListArtwork {
    std::vector<u8> rgba;
    u32 width = 0;
    u32 height = 0;
    u64 texture_key = 0;
    u64 generation = 0;
};

class VideoBackground;

struct GameListEntry {
    std::string path;
    std::string title; ///< Short title from the SMDH, or the file name as a fallback.
    std::string long_title;
    std::string file_type; ///< "CCI", "CXI", "3DSX", ...
    std::string region;
    std::string compatibility{"Not Tested"};
    u64 program_id = 0;
    u64 extdata_id = 0;
    u64 size_bytes = 0;
    u64 play_time_seconds = 0;
    Service::FS::MediaType media_type{};
    std::shared_ptr<const GameListIcon> icon;
    std::shared_ptr<const GameListArtwork> box_art;
    std::shared_ptr<const GameListArtwork> large_box_art;
    std::shared_ptr<const GameListArtwork> background;
    std::shared_ptr<const GameListArtwork> logo;
    std::optional<ScreenScraper::GameMetadata> scraped;
    bool has_icon = false;
    bool encrypted = false;
    bool can_insert = false;
    bool favorite = false;
};

struct ConfigureRequest {
    std::string path;
    u64 program_id = 0;
};

struct RemoteGameListEntry {
    u64 token = 0;
    std::string title;
    std::string long_title;
    std::string file_type;
    std::string region;
    u64 program_id = 0;
    u64 icon_key = 0;
    bool installed = false;
    bool favorite = false;
    // The path resolves remote media requests and is never accepted from the network.
    std::string path;
    std::string compatibility;
    u64 size_bytes = 0;
    u64 play_time_seconds = 0;
    std::optional<ScreenScraper::GameMetadata> scraped;
};

struct RemoteGameListSnapshot {
    u32 revision = 0;
    std::vector<RemoteGameListEntry> entries;
};

enum class RemoteLaunchResult {
    Accepted,
    StaleRevision,
    UnknownEntry,
};

struct RemoteSelection {
    u32 revision = 0;
    u64 token = 0;
    bool operator==(const RemoteSelection&) const = default;
};

/// The game list, cloned from citra_qt's GameList; scanning runs on a worker thread.
class GameList {
public:
    GameList();
    ~GameList();

    GameList(const GameList&) = delete;
    GameList& operator=(const GameList&) = delete;

    /// Returns the path of a title the user chose to boot, or an empty string.
    std::string Draw();

    void SetMenuCallback(std::function<void()> callback) {
        menu_callback = std::move(callback);
    }

    /// Rescans the configured directories; ignored while a scan is running.
    void Refresh();

    /// Cancels and joins the scanner so it cannot compete with a running title.
    void CancelScan();

    /// Resolves the title embedded in a TAS movie to the executable found by the library scan.
    std::optional<std::string> FindPathByProgramId(u64 program_id) const;

    std::optional<u64> FindProgramIdByPath(const std::string& path) const;

    void StartPlayTimeTracking(u64 program_id);
    void StopPlayTimeTracking();

    std::optional<ConfigureRequest> TakeConfigureRequest();

    RemoteGameListSnapshot GetRemoteSnapshot() const;
    RemoteLaunchResult ResolveRemoteLaunch(u32 revision, u64 token, std::string& path) const;
    std::shared_ptr<const GameListIcon> GetRemoteIcon(u32 revision, u64 icon_key) const;

    /// The highlighted title as a remote frontend identifies it.
    std::optional<RemoteSelection> GetRemoteSelection() const;

    /// Highlights the title a remote frontend chose; false when the token is unknown.
    bool SelectRemote(u64 token);

    bool IsScanning() const {
        return scanning;
    }

    bool IsScraping() const {
        return scraping_metadata;
    }

    void OpenSearch() {
        search_requested = true;
        search_open = true;
        focus_search_input = true;
    }

    bool HasOpenOverlay() const {
        return details_open || search_requested || search_open;
    }

    void ApplyLibraryPreferences();

    void ScrapeLibrary();

private:
    enum class MetadataField {
        None,
        Title,
        Description,
        ReleaseDate,
        Developer,
        Publisher,
        Genre,
        Players,
    };

    enum class ManagementAction {
        None,
        DeleteShaderCache,
        ResetPlayTime,
        DumpRomFS,
        UninstallApplication,
        UninstallUpdate,
        UninstallDlc,
        UninstallEverything,
    };

    struct ManagementResult {
        bool success = false;
        bool refresh_library = false;
        std::string message;
    };

    void ScanDirectories(std::vector<std::string> directories, bool deep_scan,
                         bool hide_without_icon, int icon_size, std::vector<u64> favorite_ids,
                         bool favorite_first);
    void ScrapeMissingMetadata(bool force_all);
    void ApplyCachedMetadata(GameListEntry& entry);
    /// Non-zero limits scale the image down to fit.
    std::shared_ptr<const GameListArtwork> LoadArtwork(const std::string& path, u32 max_width = 0,
                                                       u32 max_height = 0);
    std::shared_ptr<const GameListArtwork> LoadArtwork(const std::vector<u8>& encoded,
                                                       u64 texture_key);

    std::shared_ptr<const GameListIcon> GetCachedIcon(const std::vector<u8>& smdh_data, bool large);

    /// Presentation-thread texture management; the only methods that call GL.
    unsigned int GetOrCreateIconTexture(const GameListEntry& entry);
    unsigned int GetOrCreateArtworkTexture(const std::shared_ptr<const GameListArtwork>& artwork);
    void PruneIconTextures(bool icons_enabled);
    void ReleaseIconTextures();
    bool IsPresentationContextCurrent() const;

    void DrawLibraryBackground(ImGuiViewport* viewport, double now);
    void DrawLibraryDetails(ImDrawList* draw_list, const GameListEntry& entry, const ImVec2& origin,
                            const ImVec2& size, float margin_x, float top, float bottom,
                            float text_size, double now);
    void DrawTilePlaceholder(ImDrawList* draw_list, const GameListEntry& entry, const ImVec2& min,
                             const ImVec2& max, float alpha, float rounding, float text_size);
    float ArtworkFade(const GameListArtwork& artwork, double now);

    bool MatchesFilter(const GameListEntry& entry) const;

    void LoadCompatibilityList();
    void SortEntries();
    void ToggleFavorite(u64 program_id);
    void OpenDetails(const GameListEntry& entry);
    void DrawDetails();
    void OpenMetadataEditor(const GameListEntry& entry);
    void CloseMetadataEditor();
    void DrawMetadataEditor();
    void DrawMetadataMatchBrowser();
    void DrawMetadataFieldEditor();
    void OpenMetadataFieldEditor(MetadataField field);
    void StartMetadataSearch();
    void StartMetadataMatchDownload(const ScreenScraper::SearchMatch& match);
    void CycleMetadataMatchArtwork(int direction);
    void ResetMetadataMatchArtworkOverride();
    bool MetadataHasChanges() const;
    void PollMetadataOperations();
    bool ApplyEditedMetadata();
    void ApplyMetadata(const ScreenScraper::GameMetadata& metadata);
    void DrawManagementConfirmation();
    void SelectManagedPath(std::string label, std::string path, bool create);
    void ExecuteManagementAction();
    void StartUninstall(Service::FS::MediaType media_type, u64 title_id, std::string label);
    void StartUninstallEverything(const GameListEntry& entry);
    void StartRomFsDump(const GameListEntry& entry, bool dump_update);
    void PollManagementOperation();

    std::vector<GameListEntry> entries;
    mutable std::mutex entries_mutex;
    std::atomic<u32> remote_revision{1};

    std::thread scan_thread;
    std::thread scrape_thread;
    std::atomic<bool> scanning{false};
    std::atomic<bool> scraping_metadata{false};
    std::atomic<bool> stop_scanning{false};

    std::mutex icon_cache_mutex;
    std::unordered_map<u64, std::weak_ptr<const GameListIcon>> decoded_icon_cache;
    std::unordered_map<u64, unsigned int> icon_textures;
    std::unordered_map<u64, unsigned int> artwork_textures;
    std::unordered_map<u64, u64> artwork_uploaded_generation;
    std::atomic<bool> icon_prune_requested{true};
    std::thread::id presentation_thread;
    std::uintptr_t presentation_context = 0;
    int presented_icon_mode = -1;
    bool context_warning_logged = false;

    std::string filter_text;
    bool search_requested = false;
    bool search_open = false;
    bool focus_search_input = false;
    bool focus_game_row = true;
    bool focus_file_menu = false;
    std::unordered_map<int, float> search_row_focus_amount;
    float top_menu_alpha = 1.0f;
    std::chrono::steady_clock::time_point top_menu_last_active{};
    int last_home_frame = -1;
    int presented_list_box_art_mode = -1;
    int presented_large_box_art_mode = -1;
    int presented_background_mode = -1;
    int selected_index = -1;
    int remote_focus_index = -1;
    std::function<void()> menu_callback;

    int presented_selection = -1;
    double selection_changed_at = 0.0;
    float row_scroll = 0.0f;
    float row_focus_amount = 1.0f;
    bool shelf_press_on_selected = false;
    std::shared_ptr<const GameListArtwork> shown_background;
    std::shared_ptr<const GameListArtwork> previous_background;
    double background_changed_at = -1e9;
    double video_visible_since = -1.0;
    std::unordered_map<u64, double> artwork_first_drawn;

    PlayTime::PlayTimeManager play_time_manager;
    std::unordered_map<u64, std::string> compatibility_list;

    std::optional<GameListEntry> details_entry;
    std::optional<ConfigureRequest> configure_request;
    bool details_open = false;
    bool focus_details_action = false;
    bool metadata_editor_open = false;
    bool metadata_match_browser_open = false;
    bool focus_metadata_editor_first = false;
    bool metadata_save_prompt_requested = false;
    bool focus_metadata_search = false;
    bool focus_first_metadata_match = false;
    bool metadata_match_accept_ready = false;
    bool metadata_search_pending = false;
    bool focus_metadata_field_input = false;
    MetadataField metadata_field = MetadataField::None;
    std::string metadata_field_buffer;
    std::string metadata_search_text;
    std::string metadata_search_query;
    std::string metadata_editor_message;
    std::string metadata_preview_message;
    std::chrono::steady_clock::time_point metadata_search_changed_at{};
    ScreenScraper::GameMetadata original_metadata;
    ScreenScraper::GameMetadata edited_metadata;
    std::vector<ScreenScraper::SearchMatch> metadata_matches;
    std::vector<std::shared_ptr<const GameListArtwork>> metadata_match_thumbnails;
    std::vector<bool> metadata_thumbnail_attempted;
    std::unordered_map<int, float> metadata_match_row_focus_amount;
    int metadata_match_artwork_override = -1;
    u64 metadata_match_artwork_override_game_id = 0;
    bool metadata_match_artwork_load_pending = false;
    std::chrono::steady_clock::time_point metadata_match_artwork_changed_at{};
    u64 metadata_match_artwork_cache_game_id = 0;
    std::unordered_map<std::string, std::shared_ptr<const GameListArtwork>>
        metadata_match_artwork_cache;
    std::unordered_set<std::string> metadata_match_artwork_attempted;
    int metadata_selected_match = -1;
    int metadata_thumbnail_index = -1;
    std::shared_ptr<const GameListArtwork> metadata_match_preview;
    std::shared_ptr<const GameListArtwork> metadata_editor_artwork;
    std::future<ScreenScraper::SearchResult> metadata_search_future;
    std::future<ScreenScraper::ImageResult> metadata_thumbnail_future;
    std::future<ScreenScraper::ImageResult> metadata_preview_future;
    std::future<ScreenScraper::Result> metadata_scrape_future;
    u64 metadata_editor_session = 0;
    u64 metadata_search_session = 0;
    u64 metadata_thumbnail_session = 0;
    u64 metadata_preview_session = 0;
    u64 metadata_scrape_session = 0;
    u64 metadata_search_program_id = 0;
    u64 metadata_thumbnail_game_id = 0;
    u64 metadata_preview_game_id = 0;
    u64 metadata_preview_download_game_id = 0;
    std::string metadata_preview_download_url;
    u64 metadata_scrape_program_id = 0;
    ManagementAction pending_action = ManagementAction::None;
    std::string managed_path_label;
    std::string managed_path;
    std::string management_message;
    std::future<ManagementResult> management_future;
    u64 tracked_program_id = 0;
    bool tracking_play_time = false;
    std::unique_ptr<VideoBackground> video_background;
};

} // namespace UWP
