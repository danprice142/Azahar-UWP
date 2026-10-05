// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "core/savestate.h"

namespace UWP {

/// Every command reachable from the menu bar, mirroring the QAction set in citra_qt/main.ui.
enum class MenuAction {
    None,

    // File
    LoadFile,
    LoadRecentFile,
    ClearRecentFiles,
    InstallCIA,
    ConnectArtic,
    SetupSystemFiles,
    BootHomeMenuJPN,
    BootHomeMenuUSA,
    BootHomeMenuEUR,
    BootHomeMenuAUS,
    BootHomeMenuCHN,
    BootHomeMenuKOR,
    BootHomeMenuTWN,
    LoadAmiibo,
    RemoveAmiibo,
    OpenUserFolder,
    OpenNANDFolder,
    OpenSDMCFolder,
    EjectCartridge,
    Exit,

    // Emulation
    Pause,
    Stop,
    Restart,
    StartBroadcasts,
    StopBroadcasts,
    SaveState,
    LoadState,
    SaveToOldestSlot,
    QuickSave,
    LoadFromNewestSlot,
    QuickLoad,
    Configure,
    ConfigureCurrentGame,
    Cheats,

    // View
    SingleWindowMode,
    SearchGameList,
    ToggleFavoritesFirst,
    ToggleFavoritesOnly,
    RefreshGameList,
    ShowStatusBar,
    Fullscreen,
    ScreenLayoutDefault,
    ScreenLayoutSingleScreen,
    ScreenLayoutLargeScreen,
    ScreenLayoutSideBySide,
    ScreenLayoutSeparateWindows,
    ScreenLayoutHybridScreen,
    ScreenLayoutCustomLayout,
    ScreenLayoutSwapScreens,
    ScreenLayoutUprightScreens,
    ScreenLayoutCycle,
    SmallScreenTopRight,
    SmallScreenMiddleRight,
    SmallScreenBottomRight,
    SmallScreenTopLeft,
    SmallScreenMiddleLeft,
    SmallScreenBottomLeft,
    SmallScreenAbove,
    SmallScreenBelow,

    // Multiplayer
    ViewLobby,
    StartRoom,
    LeaveRoom,
    ConnectToRoom,
    ShowRoom,

    // Tools
    RecordMovie,
    PlayMovie,
    CloseMovie,
    SaveMovie,
    MovieReadOnlyMode,
    AdvanceFrame,
    CaptureScreenshot,
    DumpVideo,
    CompressROMFile,
    DecompressROMFile,
    OpenLogFolder,
    ScrapeGameLibrary,
    CreatePicaSurfaceViewer,
    DebugPause,
    DebugResume,
    DebugStep,
    DebugUnscheduleAll,
    DebugScheduleAll,

    // Help
    ReportCompatibility,
    FAQ,
    DiscordXboxEmulationHub,
    DiscordRevivesCommunity,
    AppStore,
    About,
};

/// Emulator state used to enable, disable and check menu items.
struct MenuContext {
    bool input_enabled = true;
    bool emulation_running = false;
    bool emulation_paused = false;
    bool movie_recording = false;
    bool movie_playing = false;
    bool movie_read_only = true;
    bool video_dumping = false;
    bool allow_broadcasting = true;
    bool main_broadcast_active = false;
    bool auxiliary_broadcast_active = false;
    bool in_room = false;
    bool room_hosted = false;
    bool fullscreen = false;
    bool show_status_bar = true;
    bool single_window_mode = true;
    bool game_list_visible = false;
    bool library_favorites_first = true;
    bool library_favorites_only = false;
    bool cartridge_inserted = false;
    bool screenscraper_ready = false;
    bool library_busy = false;
    std::array<bool, Core::SaveStateSlotCount> save_state_slots{};
};

/// The menu bar, cloned from citra_qt's MainWindow. Draw() returns the action picked, if any.
class MainMenu {
public:
    /// Draws the home-page button row in a full-width overlay while a title is running.
    MenuAction Draw(const MenuContext& context);

    /// Focuses the Emulation button the next time the bar is drawn (gameplay chord).
    void FocusEmulation() {
        focus_top_level_requested = 1;
    }

    /// Reopens the menu containing an action and restores focus to that item.
    void RestoreAction(MenuAction action);

    /// Draws just the menus; the game list hosts the bar inside its own window.
    MenuAction DrawContents(const MenuContext& context);

    /// True while a top-level dropdown is open, so B closes it before dismissing the row.
    bool HasOpenTopLevelPopup() const {
        return top_level_popup_open;
    }

    /// Slot chosen from the Save/Load State submenus.
    u32 SelectedStateSlot() const {
        return selected_state_slot;
    }

    void SetRecentFiles(std::vector<std::string> files) {
        recent_files = std::move(files);
    }

    const std::string& SelectedRecentFile() const {
        return selected_recent_file;
    }

private:
    MenuAction DrawFileMenu(const MenuContext& context);
    MenuAction DrawEmulationMenu(const MenuContext& context);
    MenuAction DrawViewMenu(const MenuContext& context);
    MenuAction DrawMultiplayerMenu(const MenuContext& context);
    MenuAction DrawToolsMenu(const MenuContext& context);
    MenuAction DrawHelpMenu(const MenuContext& context);
    bool ActionItem(const char* label, MenuAction candidate, bool enabled = true);
    void ReturnToTopLevelOnUp(int top_level, bool enabled = true);

    u32 selected_state_slot = 1;
    int focused_top_level = -1;
    int focus_top_level_requested = -1;
    int open_top_level_requested = -1;
    int last_open_top_level = -1;
    bool top_level_popup_open = false;
    MenuAction focus_action_requested = MenuAction::None;
    std::vector<std::string> recent_files;
    std::string selected_recent_file;
};

} // namespace UWP
