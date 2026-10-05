// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "common/announce_multiplayer_room.h"
#include "common/common_types.h"
#include "common/web_util.h"
#include "network/announce_multiplayer_session.h"
#include "network/room_member.h"

namespace UWP {

/// The multiplayer interface, gathering citra_qt's lobby, host, direct-connect, room, chat and
/// moderation dialogs into one class; each still draws as its own window.
class Multiplayer {
public:
    Multiplayer();
    ~Multiplayer();

    Multiplayer(const Multiplayer&) = delete;
    Multiplayer& operator=(const Multiplayer&) = delete;

    void Draw();

    void OpenLobby();
    void OpenHostRoom();
    void OpenDirectConnect();
    void OpenClientRoom();
    void OpenModeration();

    /// True while connected to or hosting a room.
    bool IsInRoom() const;

    bool IsHosting() const {
        return hosting;
    }

    bool IsWindowOpen() const {
        return show_lobby || show_host_room || show_direct_connect || show_client_room ||
               show_moderation;
    }

    void LeaveRoom();

    /// Stops outstanding announce/connect work and unbinds callbacks; call before
    /// Network::Shutdown().
    void Shutdown();

private:
    void DrawLobby();
    void DrawHostRoom();
    void DrawDirectConnect();
    void DrawClientRoom();
    void DrawModeration();

    void DrawChat();

    void BindNetworkCallbacks();
    void UnbindNetworkCallbacks();
    void DrainNetworkEvents();
    void PollAsyncOperations();
    void RefreshLobby();
    void StartJoin(const std::string& nickname, const std::string& address, u16 port,
                   const std::string& password, const std::string& verify_uid);
    void StartHosting();
    void UseRunningApplicationAsPreference();
    void ShowError(std::string message);

    bool show_lobby = false;
    bool show_host_room = false;
    bool show_direct_connect = false;
    bool show_client_room = false;
    bool show_moderation = false;

    bool hosting = false;

    // Lobby
    struct LobbyRoom {
        std::string name;
        std::string description;
        std::string owner;
        std::string preferred_game;
        std::string host;
        std::string verify_uid;
        u16 port = 0;
        u64 preferred_game_id = 0;
        int members = 0;
        int max_members = 0;
        bool has_password = false;
    };
    std::vector<LobbyRoom> lobby_rooms;
    std::string lobby_filter;
    bool lobby_hide_full = false;
    bool lobby_hide_empty = false;
    int lobby_selected = -1;
    std::array<char, 64> lobby_nickname{};
    std::array<char, 64> lobby_password{};
    bool lobby_refreshing = false;
    std::future<AnnounceMultiplayerRoom::RoomList> lobby_refresh_future;

    // Host room
    std::array<char, 128> host_room_name{};
    std::array<char, 256> host_description{};
    std::array<char, 64> host_password{};
    std::array<char, 64> host_nickname{};
    int host_port = 24872;
    int host_max_members = 8;
    int host_visibility = 0; ///< 0 = public, 1 = unlisted
    std::array<char, 128> host_preferred_game{};
    std::array<char, 24> host_preferred_game_id{};
    bool host_reuse_bans = true;
    bool host_in_progress = false;

    // Direct connect
    std::array<char, 128> direct_address{};
    std::array<char, 64> direct_nickname{};
    std::array<char, 64> direct_password{};
    int direct_port = 24872;

    // Chat
    std::array<char, 512> chat_input{};
    std::vector<std::string> chat_log;
    bool chat_scroll_to_bottom = false;
    Network::RoomMember::MemberList member_list;
    std::string selected_member;

    // Moderation
    struct BanEntry {
        std::string subject;
        std::string type;
    };
    std::vector<BanEntry> ban_list;
    Network::Room::BanList saved_ban_list;
    int moderation_selected = -1;

    // Asynchronous connection work
    struct ConnectionResult {
        bool host_created = false;
        std::string error;
    };
    std::future<ConnectionResult> connection_future;
    bool connection_in_progress = false;

    // Network callbacks
    struct PendingNetworkEvents {
        std::vector<std::string> chat_lines;
        std::optional<Network::RoomMember::State> state;
        std::optional<Network::RoomMember::Error> error;
        std::optional<Network::Room::BanList> bans;
        std::optional<Network::RoomMember::MemberList> members;
        std::optional<Common::WebResult> announce_error;
    };

    std::mutex pending_events_mutex;
    PendingNetworkEvents pending_events;
    std::weak_ptr<Network::RoomMember> bound_member;
    Network::RoomMember::CallbackHandle<Network::RoomMember::State> state_callback;
    Network::RoomMember::CallbackHandle<Network::RoomMember::Error> error_callback;
    Network::RoomMember::CallbackHandle<Network::RoomInformation> room_callback;
    Network::RoomMember::CallbackHandle<Network::ChatEntry> chat_callback;
    Network::RoomMember::CallbackHandle<Network::StatusMessageEntry> status_callback;
    Network::RoomMember::CallbackHandle<Network::Room::BanList> ban_callback;

    std::shared_ptr<Network::AnnounceMultiplayerSession> announce_session;
    Network::AnnounceMultiplayerSession::CallbackHandle announce_error_callback;

    std::string connection_status;
    std::string error_popup;
    bool shutdown = false;
};

} // namespace UWP
