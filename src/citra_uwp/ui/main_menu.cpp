// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string>
#include "citra_uwp/ui/main_menu.h"

#include <fmt/format.h>

#include "common/settings.h"
#include "core/savestate.h"

namespace UWP {

namespace {

constexpr std::array<const char*, 6> kTopLevelLabels{
    "File", "Emulation", "View", "Multiplayer", "Tools", "Help",
};

/// ImGui::MenuItem with an explicit enabled flag.
bool Item(const char* label, bool enabled = true, bool selected = false,
          const char* shortcut = nullptr) {
    return ImGui::MenuItem(label, shortcut, selected, enabled);
}

/// Opens a top-level menu as a button plus a popup; ImGui::BeginMenu outside a menu bar binds
/// Right to "open" and traps directional navigation. The caller must call EndPopup() on true.
bool BeginTopLevelMenu(const char* label, bool request_focus = false) {
    if (request_focus) {
        // NavId/NavLayer internals are only valid during ImGui's own navigation update.
        ImGui::SetKeyboardFocusHere();
    }
    if (ImGui::Button(label)) {
        ImGui::OpenPopup(label);
    }
    if (request_focus) {
        ImGui::SetItemDefaultFocus();
    }
    const ImVec2 anchor{ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y};
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Always);
    return ImGui::BeginPopup(label);
}

int OpenTopLevelPopupIndex() {
    const auto open = std::find_if(kTopLevelLabels.begin(), kTopLevelLabels.end(),
                                   [](const char* label) { return ImGui::IsPopupOpen(label); });
    return open == kTopLevelLabels.end()
               ? -1
               : static_cast<int>(std::distance(kTopLevelLabels.begin(), open));
}

int TopLevelIndexForAction(MenuAction action) {
    const auto value = static_cast<int>(action);
    if (value >= static_cast<int>(MenuAction::LoadFile) &&
        value <= static_cast<int>(MenuAction::Exit)) {
        return 0;
    }
    if (value >= static_cast<int>(MenuAction::Pause) &&
        value <= static_cast<int>(MenuAction::Cheats)) {
        return 1;
    }
    if (value >= static_cast<int>(MenuAction::SingleWindowMode) &&
        value <= static_cast<int>(MenuAction::SmallScreenBelow)) {
        return 2;
    }
    if (value >= static_cast<int>(MenuAction::ViewLobby) &&
        value <= static_cast<int>(MenuAction::ShowRoom)) {
        return 3;
    }
    if (value >= static_cast<int>(MenuAction::RecordMovie) &&
        value <= static_cast<int>(MenuAction::DebugScheduleAll)) {
        return 4;
    }
    if (value >= static_cast<int>(MenuAction::ReportCompatibility) &&
        value <= static_cast<int>(MenuAction::About)) {
        return 5;
    }
    return -1;
}

} // namespace

void MainMenu::RestoreAction(MenuAction action) {
    const int top_level = TopLevelIndexForAction(action);
    if (top_level < 0) {
        return;
    }
    focus_action_requested = action;
    open_top_level_requested = top_level;
    focus_top_level_requested = top_level;
    last_open_top_level = top_level;
}

bool MainMenu::ActionItem(const char* label, MenuAction candidate, bool enabled) {
    const bool restore_focus = focus_action_requested == candidate;
    if (restore_focus) {
        // Keep requesting focus until ImGui confirms it has crossed into the popup.
        ImGui::SetWindowFocus();
        ImGui::SetKeyboardFocusHere();
    }
    const bool activated = Item(label, enabled);
    if (restore_focus) {
        ImGui::SetItemDefaultFocus();
    }
    if (restore_focus && ImGui::IsItemFocused()) {
        focus_action_requested = MenuAction::None;
    }
    return activated;
}

void MainMenu::ReturnToTopLevelOnUp(int top_level, bool enabled) {
    if (!enabled || !ImGui::IsItemFocused() ||
        !ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false)) {
        return;
    }

    // Moving up from the first entry returns to the button row; ImGui handles nested submenus.
    ImGui::CloseCurrentPopup();
    focus_action_requested = MenuAction::None;
    open_top_level_requested = -1;
    focus_top_level_requested = top_level;
    last_open_top_level = top_level;
}

MenuAction MainMenu::DrawContents(const MenuContext& context) {
    MenuAction action = MenuAction::None;

    const auto take = [&action](MenuAction candidate) {
        if (candidate != MenuAction::None) {
            action = candidate;
        }
    };

    constexpr int top_level_count = 6;
    const bool popup_was_open = top_level_popup_open;
    const int open_popup = OpenTopLevelPopupIndex();
    const bool popup_open = open_popup >= 0;
    if (popup_open) {
        last_open_top_level = open_popup;
    }
    if (popup_was_open && !popup_open && last_open_top_level >= 0) {
        // After B closes the dropdown, return focus to its button so LB/RB can move along the row.
        focus_top_level_requested = last_open_top_level;
    }
    const bool previous = context.input_enabled && ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false);
    const bool next = context.input_enabled && ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false);
    const int shoulder_source = popup_open ? open_popup : focused_top_level;
    if (shoulder_source >= 0 && previous != next) {
        const int target = previous ? (shoulder_source + top_level_count - 1) % top_level_count
                                    : (shoulder_source + 1) % top_level_count;
        if (popup_open) {
            open_top_level_requested = target;
            last_open_top_level = target;
        } else {
            focus_top_level_requested = target;
        }
    }
    focused_top_level = -1;

    const auto submit = [&](int index, const char* label, auto&& draw) {
        const bool request_open = open_top_level_requested == index;
        if (request_open) {
            ImGui::OpenPopup(label);
            open_top_level_requested = -1;
        }
        const bool request_focus = !request_open && focus_top_level_requested == index;
        if (request_focus) {
            ImGui::SetKeyboardFocusHere();
        }
        take(draw());
        if (request_open && focus_top_level_requested == index) {
            focus_top_level_requested = -1;
        }
        if (request_focus) {
            ImGui::SetItemDefaultFocus();
            focused_top_level = index;
            focus_top_level_requested = -1;
        }
        if (!popup_open && ImGui::IsItemFocused()) {
            focused_top_level = index;
        }
    };

    // Outside a menu bar BeginMenu takes a full-width row, so SameLine lays the entries out.
    submit(0, "File", [&] { return DrawFileMenu(context); });
    ImGui::SameLine();
    submit(1, "Emulation", [&] { return DrawEmulationMenu(context); });
    ImGui::SameLine();
    submit(2, "View", [&] { return DrawViewMenu(context); });
    ImGui::SameLine();
    submit(3, "Multiplayer", [&] { return DrawMultiplayerMenu(context); });
    ImGui::SameLine();
    submit(4, "Tools", [&] { return DrawToolsMenu(context); });
    ImGui::SameLine();
    submit(5, "Help", [&] { return DrawHelpMenu(context); });

    // Popup IDs are scoped to this window, so the state must be recorded before Draw() ends.
    top_level_popup_open = OpenTopLevelPopupIndex() >= 0;

    return action;
}

MenuAction MainMenu::Draw(const MenuContext& context) {
    // BeginMainMenuBar would trap focus on ImGui's menu navigation layer; reuse the home-page
    // row.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f;
    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2{viewport->WorkSize.x, height}, ImGuiCond_Always);
    if (focus_top_level_requested >= 0) {
        ImGui::SetNextWindowFocus();
    }
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground;
    if (!ImGui::Begin("##in_game_main_menu", nullptr, flags)) {
        ImGui::End();
        return MenuAction::None;
    }
    const MenuAction action = DrawContents(context);
    ImGui::End();
    return action;
}

MenuAction MainMenu::DrawFileMenu(const MenuContext& context) {
    MenuAction action = MenuAction::None;
    if (!BeginTopLevelMenu("File")) {
        return action;
    }

    if (ActionItem("Load File...", MenuAction::LoadFile)) {
        action = MenuAction::LoadFile;
    }
    ReturnToTopLevelOnUp(0);
    if (ActionItem("Install CIA...", MenuAction::InstallCIA)) {
        action = MenuAction::InstallCIA;
    }
    if (ActionItem("Connect to Artic Base...", MenuAction::ConnectArtic)) {
        action = MenuAction::ConnectArtic;
    }
    if (ActionItem("Set Up System Files...", MenuAction::SetupSystemFiles)) {
        action = MenuAction::SetupSystemFiles;
    }

    ImGui::Separator();

    if (ImGui::BeginMenu("Boot Home Menu")) {
        struct Region {
            const char* label;
            MenuAction action;
        };
        constexpr Region regions[]{
            {"JPN", MenuAction::BootHomeMenuJPN}, {"USA", MenuAction::BootHomeMenuUSA},
            {"EUR", MenuAction::BootHomeMenuEUR}, {"AUS", MenuAction::BootHomeMenuAUS},
            {"CHN", MenuAction::BootHomeMenuCHN}, {"KOR", MenuAction::BootHomeMenuKOR},
            {"TWN", MenuAction::BootHomeMenuTWN},
        };
        for (const auto& region : regions) {
            if (Item(region.label)) {
                action = region.action;
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Recent Files", !recent_files.empty())) {
        for (std::size_t index = 0; index < recent_files.size(); ++index) {
            const std::filesystem::path path{recent_files[index]};
            std::string label = path.filename().string();
            if (label.empty()) {
                label = recent_files[index];
            }
            ImGui::PushID(static_cast<int>(index));
            if (Item(label.c_str())) {
                selected_recent_file = recent_files[index];
                action = MenuAction::LoadRecentFile;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", recent_files[index].c_str());
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        if (Item("Clear Recent Files")) {
            action = MenuAction::ClearRecentFiles;
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();

    if (focus_action_requested == MenuAction::LoadAmiibo) {
        ImGui::OpenPopup("Amiibo");
    }
    if (ImGui::BeginMenu("Amiibo", context.emulation_running)) {
        if (ActionItem("Load...", MenuAction::LoadAmiibo)) {
            action = MenuAction::LoadAmiibo;
        }
        if (Item("Remove")) {
            action = MenuAction::RemoveAmiibo;
        }
        ImGui::EndMenu();
    }

    if (Item("Eject Inserted Cartridge", context.cartridge_inserted)) {
        action = MenuAction::EjectCartridge;
    }

    if (ImGui::BeginMenu("Open Azahar Folder")) {
        if (Item("User Folder")) {
            action = MenuAction::OpenUserFolder;
        }
        if (Item("NAND Folder")) {
            action = MenuAction::OpenNANDFolder;
        }
        if (Item("SDMC Folder")) {
            action = MenuAction::OpenSDMCFolder;
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    if (Item("Exit")) {
        action = MenuAction::Exit;
    }

    ImGui::EndPopup();
    return action;
}

MenuAction MainMenu::DrawEmulationMenu(const MenuContext& context) {
    MenuAction action = MenuAction::None;
    if (!BeginTopLevelMenu("Emulation")) {
        return action;
    }

    const bool running = context.emulation_running;
    const bool has_numbered_state =
        std::any_of(context.save_state_slots.begin() + 1, context.save_state_slots.end(),
                    [](bool occupied) { return occupied; });

    if (Item(context.emulation_paused ? "Continue" : "Pause", running)) {
        action = MenuAction::Pause;
    }
    ReturnToTopLevelOnUp(1, running);
    if (Item("Stop", running)) {
        action = MenuAction::Stop;
    }
    if (Item("Restart", running)) {
        action = MenuAction::Restart;
    }

    ImGui::Separator();

    const bool any_broadcast_active =
        context.main_broadcast_active || context.auxiliary_broadcast_active;
    const bool all_broadcasts_active =
        context.main_broadcast_active && context.auxiliary_broadcast_active;
    if (Item("Start Broadcasts", running && context.allow_broadcasting && !all_broadcasts_active)) {
        action = MenuAction::StartBroadcasts;
    }
    if (Item("Stop Broadcasts", any_broadcast_active)) {
        action = MenuAction::StopBroadcasts;
    }

    ImGui::Separator();

    // Slot 0 is the quick slot, so the numbered submenus start at 1, as in the Qt build.
    if (ImGui::BeginMenu("Save State", running)) {
        for (u32 slot = 1; slot < Core::SaveStateSlotCount; ++slot) {
            const std::string label = fmt::format("Slot {}", slot);
            if (Item(label.c_str())) {
                selected_state_slot = slot;
                action = MenuAction::SaveState;
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Load State", running)) {
        for (u32 slot = 1; slot < Core::SaveStateSlotCount; ++slot) {
            const std::string label = fmt::format("Slot {}", slot);
            if (Item(label.c_str(), context.save_state_slots[slot])) {
                selected_state_slot = slot;
                action = MenuAction::LoadState;
            }
        }
        ImGui::EndMenu();
    }
    if (Item("Save to Oldest Slot", running)) {
        action = MenuAction::SaveToOldestSlot;
    }
    if (Item("Quick Save", running)) {
        action = MenuAction::QuickSave;
    }
    if (Item("Load from Newest Slot", running && has_numbered_state)) {
        action = MenuAction::LoadFromNewestSlot;
    }
    if (Item("Quick Load", running && context.save_state_slots[0])) {
        action = MenuAction::QuickLoad;
    }

    ImGui::Separator();
    if (ActionItem("Configure...", MenuAction::Configure)) {
        action = MenuAction::Configure;
    }
    ReturnToTopLevelOnUp(1, !running);
    if (ActionItem("Configure Current Application...", MenuAction::ConfigureCurrentGame, running)) {
        action = MenuAction::ConfigureCurrentGame;
    }
    if (ActionItem("Cheats...", MenuAction::Cheats, running)) {
        action = MenuAction::Cheats;
    }

    ImGui::EndPopup();
    return action;
}

MenuAction MainMenu::DrawViewMenu(const MenuContext& context) {
    MenuAction action = MenuAction::None;
    if (!BeginTopLevelMenu("View")) {
        return action;
    }

    if (Item("Single Window Mode", false, context.single_window_mode)) {
        action = MenuAction::SingleWindowMode;
    }
    if (focus_action_requested == MenuAction::SearchGameList) {
        ImGui::OpenPopup("Game List");
    }
    const bool game_list_open = ImGui::BeginMenu("Game List", context.game_list_visible);
    if (!game_list_open) {
        ReturnToTopLevelOnUp(2, context.game_list_visible);
    }
    if (game_list_open) {
        if (ActionItem("Search Games...", MenuAction::SearchGameList)) {
            action = MenuAction::SearchGameList;
        }
        if (Item("Favorites First", true, context.library_favorites_first)) {
            action = MenuAction::ToggleFavoritesFirst;
        }
        if (Item("Favorites Only", true, context.library_favorites_only)) {
            action = MenuAction::ToggleFavoritesOnly;
        }
        ImGui::Separator();
        if (Item("Refresh Library", !context.library_busy)) {
            action = MenuAction::RefreshGameList;
        }
        ImGui::EndMenu();
    }
    if (Item("Show Status Bar", true, context.show_status_bar)) {
        action = MenuAction::ShowStatusBar;
    }
    ReturnToTopLevelOnUp(2, !context.game_list_visible);
    if (Item("Fullscreen", false, context.fullscreen)) {
        action = MenuAction::Fullscreen;
    }

    ImGui::Separator();

    if (ImGui::BeginMenu("Screen Layout")) {
        const auto current = Settings::values.layout_option.GetValue();
        struct LayoutEntry {
            const char* label;
            Settings::LayoutOption layout;
            MenuAction action;
        };
        constexpr LayoutEntry layouts[]{
            {"Default", Settings::LayoutOption::Default, MenuAction::ScreenLayoutDefault},
            {"Single Screen", Settings::LayoutOption::SingleScreen,
             MenuAction::ScreenLayoutSingleScreen},
            {"Large Screen", Settings::LayoutOption::LargeScreen,
             MenuAction::ScreenLayoutLargeScreen},
            {"Side by Side", Settings::LayoutOption::SideScreen,
             MenuAction::ScreenLayoutSideBySide},
            {"Hybrid Screen", Settings::LayoutOption::HybridScreen,
             MenuAction::ScreenLayoutHybridScreen},
            // Separate Windows is absent: an app container has exactly one window.
            {"Custom Layout", Settings::LayoutOption::CustomLayout,
             MenuAction::ScreenLayoutCustomLayout},
        };
        for (const auto& entry : layouts) {
            if (Item(entry.label, true, current == entry.layout)) {
                action = entry.action;
            }
        }

        ImGui::Separator();
        if (Item("Swap Screens", true, Settings::values.swap_screen.GetValue())) {
            action = MenuAction::ScreenLayoutSwapScreens;
        }
        if (Item("Rotate Upright", true, Settings::values.upright_screen.GetValue())) {
            action = MenuAction::ScreenLayoutUprightScreens;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Small Screen Position")) {
        const auto current = Settings::values.small_screen_position.GetValue();
        struct PositionEntry {
            const char* label;
            Settings::SmallScreenPosition position;
            MenuAction action;
        };
        constexpr PositionEntry positions[]{
            {"Top Right", Settings::SmallScreenPosition::TopRight, MenuAction::SmallScreenTopRight},
            {"Middle Right", Settings::SmallScreenPosition::MiddleRight,
             MenuAction::SmallScreenMiddleRight},
            {"Bottom Right", Settings::SmallScreenPosition::BottomRight,
             MenuAction::SmallScreenBottomRight},
            {"Top Left", Settings::SmallScreenPosition::TopLeft, MenuAction::SmallScreenTopLeft},
            {"Middle Left", Settings::SmallScreenPosition::MiddleLeft,
             MenuAction::SmallScreenMiddleLeft},
            {"Bottom Left", Settings::SmallScreenPosition::BottomLeft,
             MenuAction::SmallScreenBottomLeft},
            {"Above", Settings::SmallScreenPosition::AboveLarge, MenuAction::SmallScreenAbove},
            {"Below", Settings::SmallScreenPosition::BelowLarge, MenuAction::SmallScreenBelow},
        };
        for (const auto& entry : positions) {
            if (Item(entry.label, true, current == entry.position)) {
                action = entry.action;
            }
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();

    if (ImGui::BeginMenu("Debugging")) {
        // The Qt dock widgets have their equivalents in the Tools > Debug submenu.
        ImGui::TextDisabled("Debug views live under Tools > Debug");
        ImGui::EndMenu();
    }

    ImGui::EndPopup();
    return action;
}

MenuAction MainMenu::DrawMultiplayerMenu(const MenuContext& context) {
    MenuAction action = MenuAction::None;
    if (!BeginTopLevelMenu("Multiplayer")) {
        return action;
    }

    if (ActionItem("Browse Public Rooms", MenuAction::ViewLobby)) {
        action = MenuAction::ViewLobby;
    }
    ReturnToTopLevelOnUp(3);
    if (ActionItem("Create Room", MenuAction::StartRoom, !context.in_room)) {
        action = MenuAction::StartRoom;
    }
    if (ActionItem("Direct Connect to Room", MenuAction::ConnectToRoom, !context.in_room)) {
        action = MenuAction::ConnectToRoom;
    }
    ImGui::Separator();
    if (ActionItem("Show Current Room", MenuAction::ShowRoom, context.in_room)) {
        action = MenuAction::ShowRoom;
    }
    if (Item("Leave Room", context.in_room)) {
        action = MenuAction::LeaveRoom;
    }

    ImGui::EndPopup();
    return action;
}

MenuAction MainMenu::DrawToolsMenu(const MenuContext& context) {
    MenuAction action = MenuAction::None;
    if (!BeginTopLevelMenu("Tools")) {
        return action;
    }

    const bool running = context.emulation_running;
    const bool movie_active = context.movie_recording || context.movie_playing;

    if (focus_action_requested == MenuAction::RecordMovie ||
        focus_action_requested == MenuAction::PlayMovie) {
        ImGui::OpenPopup("Movie");
    }
    const bool movie_menu_open = ImGui::BeginMenu("Movie");
    if (!movie_menu_open) {
        ReturnToTopLevelOnUp(4);
    }
    if (movie_menu_open) {
        if (ActionItem("Record...", MenuAction::RecordMovie, !movie_active)) {
            action = MenuAction::RecordMovie;
        }
        if (ActionItem("Play...", MenuAction::PlayMovie, !movie_active)) {
            action = MenuAction::PlayMovie;
        }
        if (Item("Close", movie_active)) {
            action = MenuAction::CloseMovie;
        }
        if (Item("Save without Closing", context.movie_recording)) {
            action = MenuAction::SaveMovie;
        }
        ImGui::Separator();
        if (Item("Read-Only Mode", true, context.movie_read_only)) {
            action = MenuAction::MovieReadOnlyMode;
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    if (Item("Advance Frame", running && context.emulation_paused)) {
        action = MenuAction::AdvanceFrame;
    }
    if (Item("Capture Screenshot", running)) {
        action = MenuAction::CaptureScreenshot;
    }
    if (ActionItem(context.video_dumping ? "Stop Video Dumping..." : "Dump Video...",
                   MenuAction::DumpVideo, running)) {
        action = MenuAction::DumpVideo;
    }

    ImGui::Separator();
    if (ActionItem("Compress ROM File...", MenuAction::CompressROMFile)) {
        action = MenuAction::CompressROMFile;
    }
    if (ActionItem("Decompress ROM File...", MenuAction::DecompressROMFile)) {
        action = MenuAction::DecompressROMFile;
    }

    if (ImGui::BeginMenu("Game Library", context.game_list_visible)) {
        if (Item("Download / Update Artwork & Descriptions",
                 context.screenscraper_ready && !context.library_busy)) {
            action = MenuAction::ScrapeGameLibrary;
        }
        if (!context.screenscraper_ready) {
            ImGui::TextDisabled("Enable library data in Configure > UI");
            ImGui::TextDisabled("and sign in under Configure > Network");
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    if (Item("Open Log Folder")) {
        action = MenuAction::OpenLogFolder;
    }

    ImGui::Separator();
    if (ImGui::BeginMenu("Debug", running)) {
        if (Item("Debug Pause")) {
            action = MenuAction::DebugPause;
        }
        if (Item("Debug Resume")) {
            action = MenuAction::DebugResume;
        }
        if (Item("Debug Step")) {
            action = MenuAction::DebugStep;
        }
        ImGui::Separator();
        if (Item("Debug Unschedule All")) {
            action = MenuAction::DebugUnscheduleAll;
        }
        if (Item("Debug Schedule All")) {
            action = MenuAction::DebugScheduleAll;
        }
        ImGui::EndMenu();
    }
    // The Pica surface viewer is a Qt debugging widget with no console equivalent.

    ImGui::EndPopup();
    return action;
}

MenuAction MainMenu::DrawHelpMenu(const MenuContext& context) {
    MenuAction action = MenuAction::None;
    if (!BeginTopLevelMenu("Help")) {
        return action;
    }

    if (Item("Report Compatibility", context.emulation_running)) {
        action = MenuAction::ReportCompatibility;
    }
    ReturnToTopLevelOnUp(5, context.emulation_running);
    ImGui::Separator();
    if (Item("FAQ")) {
        action = MenuAction::FAQ;
    }
    ReturnToTopLevelOnUp(5, !context.emulation_running);
    if (ImGui::BeginMenu("Discord")) {
        if (Item("Xbox Emulation Hub")) {
            action = MenuAction::DiscordXboxEmulationHub;
        }
        if (Item("Revives Community Server")) {
            action = MenuAction::DiscordRevivesCommunity;
        }
        ImGui::EndMenu();
    }
    if (Item("App Store")) {
        action = MenuAction::AppStore;
    }
    ImGui::Separator();
    if (ActionItem("About Azahar", MenuAction::About)) {
        action = MenuAction::About;
    }

    ImGui::EndPopup();
    return action;
}

} // namespace UWP
