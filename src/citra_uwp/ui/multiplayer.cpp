// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <imgui.h>

#include "citra_uwp/ui/multiplayer.h"
#include "citra_uwp/ui/virtual_keyboard.h"
#include "citra_uwp/ui/widgets.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/loader/loader.h"
#include "network/network.h"
#include "network/verify_user.h"
#ifdef ENABLE_WEB_SERVICE
#include "web_service/verify_user_jwt.h"
#include "web_service/web_backend.h"
#endif

namespace UWP {

namespace {

using namespace Widgets;
using namespace std::chrono_literals;

template <std::size_t Size>
void CopyToBuffer(std::array<char, Size>& destination, std::string_view source) {
    destination.fill('\0');
    const std::size_t length = std::min(source.size(), destination.size() - 1);
    std::copy_n(source.begin(), length, destination.begin());
}

std::string ProfileNickname() {
    try {
        std::string nickname = Service::CFG::GetUsername(Core::System::GetInstance());
        if (!nickname.empty()) {
            return nickname;
        }
    } catch (const std::exception& error) {
        LOG_WARNING(Network, "Could not read the system nickname: {}", error.what());
    }
    return "Azahar";
}

struct ConsoleIdentity {
    std::string id_hash;
    Network::MacAddress mac = Network::NoPreferredMac;
};

ConsoleIdentity GetConsoleIdentity() {
    ConsoleIdentity identity;
    try {
        Core::System& system = Core::System::GetInstance();
        identity.id_hash = Service::CFG::GetConsoleIdHash(system);
        identity.mac = Service::CFG::GetConsoleMacAddress(system);
    } catch (const std::exception& error) {
        // An empty hash would make every anonymous client collide; let the room reject it.
        LOG_ERROR(Network, "Could not read the console network identity: {}", error.what());
    }
    return identity;
}

const char* StateText(Network::RoomMember::State state) {
    switch (state) {
    case Network::RoomMember::State::Idle:
        return "Not connected";
    case Network::RoomMember::State::Joining:
        return "Connecting...";
    case Network::RoomMember::State::Joined:
        return "Connected";
    case Network::RoomMember::State::Moderator:
        return "Connected (moderator)";
    case Network::RoomMember::State::Uninitialized:
    default:
        return "Unavailable";
    }
}

std::string ErrorText(Network::RoomMember::Error error) {
    switch (error) {
    case Network::RoomMember::Error::LostConnection:
        return "The connection to the room was lost.";
    case Network::RoomMember::Error::HostKicked:
        return "The room host kicked this console.";
    case Network::RoomMember::Error::CouldNotConnect:
        return "The room did not respond. Check its address, port, and network connection.";
    case Network::RoomMember::Error::NameCollision:
        return "That nickname is already in use in this room.";
    case Network::RoomMember::Error::MacCollision:
        return "Another member in this room is using the same console MAC address.";
    case Network::RoomMember::Error::ConsoleIdCollision:
        return "Another member in this room is using the same console identity.";
    case Network::RoomMember::Error::RoomIsFull:
        return "The room is full.";
    case Network::RoomMember::Error::WrongPassword:
        return "The room password is incorrect.";
    case Network::RoomMember::Error::WrongVersion:
        return "The room is using an incompatible Azahar network version.";
    case Network::RoomMember::Error::HostBanned:
        return "This account or address is banned from the room.";
    case Network::RoomMember::Error::PermissionDenied:
        return "The room denied that moderation request.";
    case Network::RoomMember::Error::NoSuchUser:
        return "The selected room member no longer exists.";
    case Network::RoomMember::Error::UnknownError:
    default:
        return "The multiplayer connection failed.";
    }
}

std::string StatusText(const Network::StatusMessageEntry& status) {
    std::string name = status.nickname;
    if (!status.username.empty() && status.username != status.nickname) {
        name = fmt::format("{} ({})", status.nickname, status.username);
    }

    switch (status.type) {
    case Network::IdMemberJoin:
        return fmt::format("* {} has joined", name);
    case Network::IdMemberLeave:
        return fmt::format("* {} has left", name);
    case Network::IdMemberKicked:
        return fmt::format("* {} has been kicked", name);
    case Network::IdMemberBanned:
        return fmt::format("* {} has been banned", name);
    case Network::IdAddressUnbanned:
        return fmt::format("* {} has been unbanned", name);
    }
    return {};
}

Network::RoomMember::State CurrentState() {
    if (const auto member = Network::GetRoomMember().lock()) {
        return member->GetState();
    }
    return Network::RoomMember::State::Uninitialized;
}

bool ContainsInsensitive(std::string_view value, std::string_view filter) {
    if (filter.empty()) {
        return true;
    }
    const auto equal = [](char left, char right) {
        return std::tolower(static_cast<unsigned char>(left)) ==
               std::tolower(static_cast<unsigned char>(right));
    };
    return std::search(value.begin(), value.end(), filter.begin(), filter.end(), equal) !=
           value.end();
}

std::optional<u64> ParseTitleId(std::string_view value) {
    if (value.empty()) {
        return u64{0};
    }
    if (value.starts_with("0x") || value.starts_with("0X")) {
        value.remove_prefix(2);
    }
    if (value.empty()) {
        return std::nullopt;
    }

    u64 result = 0;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), result, 16);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

template <typename T>
bool FutureReady(std::future<T>& future) {
    return future.valid() && future.wait_for(0s) == std::future_status::ready;
}

/// Public rooms verify members through the web service, as in citra_qt.
std::unique_ptr<Network::VerifyUser::Backend> CreateVerifyBackend(
    [[maybe_unused]] bool use_validation) {
#ifdef ENABLE_WEB_SERVICE
    if (use_validation) {
        return std::make_unique<WebService::VerifyUserJWT>(Settings::values.web_api_url.GetValue());
    }
#endif
    return std::make_unique<Network::VerifyUser::NullBackend>();
}

std::string GetExternalJWT([[maybe_unused]] const std::string& verify_uid,
                           [[maybe_unused]] const std::string& nickname) {
#ifdef ENABLE_WEB_SERVICE
    if (verify_uid.empty() || nickname.empty() ||
        Settings::values.network_token.GetValue().empty()) {
        return {};
    }
    WebService::Client client(Settings::values.web_api_url.GetValue(), nickname,
                              Settings::values.network_token.GetValue());
    std::string token = client.GetExternalJWT(verify_uid).returned_data;
    if (token.empty()) {
        LOG_ERROR(WebService, "Could not get external JWT, verification may fail");
    } else {
        LOG_INFO(WebService, "Successfully requested external JWT: size={}", token.size());
    }
    return token;
#else
    return {};
#endif
}

} // namespace

Multiplayer::Multiplayer() {
    const std::string nickname = ProfileNickname();
    CopyToBuffer(lobby_nickname, nickname);
    CopyToBuffer(host_nickname, nickname);
    CopyToBuffer(direct_nickname, nickname);
    CopyToBuffer(host_preferred_game, "%none%");
    connection_status = "Not connected";

    announce_session = std::make_shared<Network::AnnounceMultiplayerSession>(nickname);
    announce_error_callback =
        announce_session->BindErrorCallback([this](const Common::WebResult& result) {
            std::scoped_lock lock{pending_events_mutex};
            pending_events.announce_error = result;
        });
    BindNetworkCallbacks();
}

Multiplayer::~Multiplayer() {
    Shutdown();
}

void Multiplayer::OpenLobby() {
    show_lobby = true;
    if (lobby_rooms.empty() && !lobby_refreshing) {
        RefreshLobby();
    }
}

void Multiplayer::OpenHostRoom() {
    show_host_room = true;
    if (host_nickname[0] == '\0') {
        CopyToBuffer(host_nickname, ProfileNickname());
    }
}

void Multiplayer::OpenDirectConnect() {
    show_direct_connect = true;
    if (direct_nickname[0] == '\0') {
        CopyToBuffer(direct_nickname, ProfileNickname());
    }
}

void Multiplayer::OpenClientRoom() {
    if (IsInRoom()) {
        show_client_room = true;
    } else {
        OpenLobby();
    }
}

void Multiplayer::OpenModeration() {
    if (CurrentState() != Network::RoomMember::State::Moderator) {
        ShowError("Only a room moderator can open the moderation panel.");
        return;
    }
    show_moderation = true;
    moderation_selected = -1;
    if (const auto member = Network::GetRoomMember().lock()) {
        member->RequestBanList();
    }
}

bool Multiplayer::IsInRoom() const {
    const auto state = CurrentState();
    return state == Network::RoomMember::State::Joined ||
           state == Network::RoomMember::State::Moderator;
}

void Multiplayer::LeaveRoom() {
    if (announce_session) {
        announce_session->Stop();
    }
    if (const auto member = Network::GetRoomMember().lock()) {
        if (member->IsConnected()) {
            member->Leave();
        }
    }
    if (const auto room = Network::GetRoom().lock();
        room && room->GetState() == Network::Room::State::Open) {
        saved_ban_list = room->GetBanList();
        room->Destroy();
    }

    hosting = false;
    host_in_progress = false;
    show_client_room = false;
    show_moderation = false;
    member_list.clear();
    selected_member.clear();
    ban_list.clear();
    moderation_selected = -1;
    chat_log.clear();
    connection_status = "Not connected";
}

void Multiplayer::Shutdown() {
    if (shutdown) {
        return;
    }
    shutdown = true;

    // The tasks must finish before ENet and the announce backend are destroyed.
    try {
        if (lobby_refresh_future.valid()) {
            lobby_refresh_future.wait();
            static_cast<void>(lobby_refresh_future.get());
        }
        if (connection_future.valid()) {
            connection_future.wait();
            static_cast<void>(connection_future.get());
        }
    } catch (const std::exception& error) {
        LOG_ERROR(Network, "Multiplayer shutdown task failed: {}", error.what());
    }

    LeaveRoom();
    UnbindNetworkCallbacks();
    if (announce_session && announce_error_callback) {
        announce_session->UnbindErrorCallback(announce_error_callback);
        announce_error_callback.reset();
    }
    announce_session.reset();
}

void Multiplayer::BindNetworkCallbacks() {
    const auto member = Network::GetRoomMember().lock();
    if (!member) {
        return;
    }
    if (bound_member.lock() == member && state_callback) {
        return;
    }
    UnbindNetworkCallbacks();
    bound_member = member;

    state_callback = member->BindOnStateChanged([this](const Network::RoomMember::State& state) {
        std::scoped_lock lock{pending_events_mutex};
        pending_events.state = state;
        if (state == Network::RoomMember::State::Joined ||
            state == Network::RoomMember::State::Moderator) {
            if (const auto current_member = Network::GetRoomMember().lock()) {
                pending_events.members = current_member->GetMemberInformation();
            }
        }
    });
    error_callback = member->BindOnError([this](const Network::RoomMember::Error& error) {
        std::scoped_lock lock{pending_events_mutex};
        pending_events.error = error;
    });
    room_callback =
        member->BindOnRoomInformationChanged([this](const Network::RoomInformation& /*room*/) {
            std::scoped_lock lock{pending_events_mutex};
            if (const auto current_member = Network::GetRoomMember().lock()) {
                pending_events.members = current_member->GetMemberInformation();
            }
        });
    chat_callback = member->BindOnChatMessageRecieved([this](const Network::ChatEntry& chat) {
        if (chat.message.empty()) {
            return;
        }
        std::string name = chat.nickname;
        if (!chat.username.empty() && chat.username != chat.nickname) {
            name = fmt::format("{} ({})", chat.nickname, chat.username);
        }
        std::scoped_lock lock{pending_events_mutex};
        pending_events.chat_lines.emplace_back(fmt::format("{}: {}", name, chat.message));
    });
    status_callback =
        member->BindOnStatusMessageReceived([this](const Network::StatusMessageEntry& status) {
            const std::string line = StatusText(status);
            if (line.empty()) {
                return;
            }
            std::scoped_lock lock{pending_events_mutex};
            pending_events.chat_lines.emplace_back(line);
        });
    ban_callback = member->BindOnBanListReceived([this](const Network::Room::BanList& bans) {
        std::scoped_lock lock{pending_events_mutex};
        pending_events.bans = bans;
    });
}

void Multiplayer::UnbindNetworkCallbacks() {
    if (const auto member = bound_member.lock()) {
        if (state_callback) {
            member->Unbind(state_callback);
        }
        if (error_callback) {
            member->Unbind(error_callback);
        }
        if (room_callback) {
            member->Unbind(room_callback);
        }
        if (chat_callback) {
            member->Unbind(chat_callback);
        }
        if (status_callback) {
            member->Unbind(status_callback);
        }
        if (ban_callback) {
            member->Unbind(ban_callback);
        }
    }
    bound_member.reset();
    state_callback.reset();
    error_callback.reset();
    room_callback.reset();
    chat_callback.reset();
    status_callback.reset();
    ban_callback.reset();
}

void Multiplayer::DrainNetworkEvents() {
    PendingNetworkEvents events;
    {
        std::scoped_lock lock{pending_events_mutex};
        events = std::move(pending_events);
        pending_events = {};
    }

    if (events.members) {
        member_list = std::move(*events.members);
    }
    for (auto& line : events.chat_lines) {
        chat_log.emplace_back(std::move(line));
        chat_scroll_to_bottom = true;
    }
    constexpr std::size_t MaxChatLines = 500;
    if (chat_log.size() > MaxChatLines) {
        chat_log.erase(chat_log.begin(), chat_log.begin() + static_cast<std::ptrdiff_t>(
                                                                chat_log.size() - MaxChatLines));
    }

    if (events.bans) {
        if (hosting) {
            saved_ban_list = *events.bans;
        }
        ban_list.clear();
        for (const auto& username : events.bans->first) {
            ban_list.push_back({username, "Account"});
        }
        for (const auto& address : events.bans->second) {
            ban_list.push_back({address, "IP address"});
        }
        moderation_selected = -1;
    }

    if (events.state) {
        connection_status = StateText(*events.state);
        LOG_INFO(Network, "Multiplayer state changed to {}", Network::GetStateStr(*events.state));
        if (*events.state == Network::RoomMember::State::Joined ||
            *events.state == Network::RoomMember::State::Moderator) {
            show_client_room = true;
            show_direct_connect = false;
            show_host_room = false;
            show_lobby = false;
            connection_in_progress = false;
            host_in_progress = false;
        } else if (*events.state == Network::RoomMember::State::Idle && !connection_in_progress) {
            show_client_room = false;
            show_moderation = false;
            if (hosting) {
                if (announce_session) {
                    announce_session->Stop();
                }
                if (const auto room = Network::GetRoom().lock();
                    room && room->GetState() == Network::Room::State::Open) {
                    saved_ban_list = room->GetBanList();
                    room->Destroy();
                }
                hosting = false;
            }
        }
    }

    if (events.error) {
        ShowError(ErrorText(*events.error));
        if (hosting) {
            if (announce_session) {
                announce_session->Stop();
            }
            if (const auto room = Network::GetRoom().lock();
                room && room->GetState() == Network::Room::State::Open) {
                saved_ban_list = room->GetBanList();
                room->Destroy();
            }
            hosting = false;
        }
    }

    if (events.announce_error) {
        if (announce_session) {
            announce_session->Stop();
        }
        ShowError(fmt::format("The public room announcement failed: {}",
                              events.announce_error->result_string));
    }
}

void Multiplayer::PollAsyncOperations() {
    if (FutureReady(lobby_refresh_future)) {
        lobby_refreshing = false;
        try {
            const auto announced_rooms = lobby_refresh_future.get();
            lobby_rooms.clear();
            lobby_rooms.reserve(announced_rooms.size());
            for (const auto& room : announced_rooms) {
                LobbyRoom lobby_room;
                lobby_room.name = room.name;
                lobby_room.description = room.description;
                lobby_room.owner = room.owner;
                lobby_room.preferred_game =
                    room.preferred_game.empty() || room.preferred_game == "%none%"
                        ? "No Preference"
                        : room.preferred_game;
                lobby_room.host = room.ip;
                lobby_room.verify_uid = room.verify_UID;
                lobby_room.port = room.port;
                lobby_room.preferred_game_id = room.preferred_game_id;
                lobby_room.members = static_cast<int>(room.members.size());
                lobby_room.max_members = static_cast<int>(room.max_player);
                lobby_room.has_password = room.has_password;
                lobby_rooms.emplace_back(std::move(lobby_room));
            }
            lobby_selected = -1;
            connection_status = fmt::format("Found {} public room{}", lobby_rooms.size(),
                                            lobby_rooms.size() == 1 ? "" : "s");
        } catch (const std::exception& error) {
            ShowError(fmt::format("Could not refresh the public room list: {}", error.what()));
        }
    }

    if (FutureReady(connection_future)) {
        connection_in_progress = false;
        host_in_progress = false;
        try {
            ConnectionResult result = connection_future.get();
            if (!result.error.empty()) {
                ShowError(std::move(result.error));
                connection_status = "Not connected";
            } else if (result.host_created) {
                hosting = true;
                connection_status = "Waiting for the room to accept the host...";
            }
        } catch (const std::exception& error) {
            ShowError(fmt::format("The multiplayer operation failed: {}", error.what()));
            connection_status = "Not connected";
        }
    }
}

void Multiplayer::RefreshLobby() {
    if (lobby_refreshing || !announce_session) {
        return;
    }
    if (announce_session->IsRunning()) {
        ShowError("Leave the room you are hosting before browsing public rooms.");
        return;
    }

    lobby_refreshing = true;
    connection_status = "Refreshing public rooms...";
    const auto session = announce_session;
    lobby_refresh_future =
        std::async(std::launch::async, [session] { return session->GetRoomList(); });
}

void Multiplayer::StartJoin(const std::string& nickname, const std::string& address, u16 port,
                            const std::string& password, const std::string& verify_uid) {
    if (connection_in_progress) {
        return;
    }
    const auto member = Network::GetRoomMember().lock();
    if (!member) {
        ShowError("Multiplayer networking is not initialized.");
        return;
    }
    if (member->IsConnected()) {
        ShowError("Leave the current room before joining another one.");
        return;
    }

    const ConsoleIdentity identity = GetConsoleIdentity();
    if (identity.id_hash.empty()) {
        ShowError("The console identity could not be read.");
        return;
    }

    connection_in_progress = true;
    connection_status = "Connecting...";
    connection_future = std::async(
        std::launch::async,
        [member, nickname, address, port, password, verify_uid, identity]() -> ConnectionResult {
            const std::string token = GetExternalJWT(verify_uid, nickname);
            member->Join(nickname, identity.id_hash, address.c_str(), port, 0, identity.mac,
                         password, token);
            if (member->GetState() == Network::RoomMember::State::Idle) {
                return {.error = "The room did not respond. Check its address and port."};
            }
            return {};
        });
}

void Multiplayer::StartHosting() {
    if (connection_in_progress || host_in_progress) {
        return;
    }
    if (lobby_refreshing) {
        ShowError("Wait for the public room refresh to finish before hosting.");
        return;
    }

    const auto room = Network::GetRoom().lock();
    const auto member = Network::GetRoomMember().lock();
    if (!room || !member || !announce_session) {
        ShowError("Multiplayer networking is not initialized.");
        return;
    }
    if (member->IsConnected() || room->GetState() == Network::Room::State::Open) {
        ShowError("Leave the current room before creating another one.");
        return;
    }

    const auto preferred_game_id = ParseTitleId(host_preferred_game_id.data());
    if (!preferred_game_id) {
        ShowError("Preferred Title ID must be a hexadecimal number.");
        return;
    }

    const ConsoleIdentity identity = GetConsoleIdentity();
    if (identity.id_hash.empty()) {
        ShowError("The console identity could not be read.");
        return;
    }

    const std::string nickname = host_nickname.data();
    const std::string room_name = host_room_name.data();
    const std::string description = host_description.data();
    const std::string password = host_password.data();
    const std::string preferred_game =
        host_preferred_game[0] == '\0' ? "%none%" : host_preferred_game.data();
    const u16 port = static_cast<u16>(host_port);
    const u32 max_members = static_cast<u32>(host_max_members);
    const bool public_room = host_visibility == 0;
    const Network::Room::BanList initial_bans =
        host_reuse_bans ? saved_ban_list : Network::Room::BanList{};

    announce_session->UpdateCredentials(nickname);
    const auto session = announce_session;
    host_in_progress = true;
    connection_in_progress = true;
    connection_status = public_room ? "Creating and announcing room..." : "Creating room...";

    connection_future = std::async(
        std::launch::async,
        [room, member, session, identity, nickname, room_name, description, password,
         preferred_game, preferred_game_id = *preferred_game_id, port, max_members, public_room,
         initial_bans]() -> ConnectionResult {
            const bool created = room->Create(
                room_name, description, "", port, password, max_members, nickname, preferred_game,
                preferred_game_id, CreateVerifyBackend(public_room), initial_bans);
            if (!created) {
                return {.error = fmt::format("Could not create a room on port {}.", port)};
            }

            std::string token;
            if (public_room) {
                const Common::WebResult result = session->Register();
                if (result.result_code != Common::WebResult::Code::Success) {
                    room->Destroy();
                    return {.error = fmt::format("Could not announce the public room: {}",
                                                 result.result_string)};
                }
                session->Start();
                token = GetExternalJWT(room->GetVerifyUID(), nickname);
            }

            member->Join(nickname, identity.id_hash, "127.0.0.1", port, 0, identity.mac, password,
                         token);
            if (member->GetState() == Network::RoomMember::State::Idle) {
                session->Stop();
                room->Destroy();
                return {.error = "The host could not join its newly created room."};
            }
            return {.host_created = true};
        });
}

void Multiplayer::UseRunningApplicationAsPreference() {
    Core::System& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        ShowError("Start an application before using it as the room preference.");
        return;
    }

    std::string title;
    u64 title_id = 0;
    if (system.GetAppLoader().ReadTitle(title) != Loader::ResultStatus::Success || title.empty()) {
        title = "Current Application";
    }
    if (system.GetAppLoader().ReadProgramId(title_id) != Loader::ResultStatus::Success) {
        title_id = 0;
    }
    CopyToBuffer(host_preferred_game, title);
    CopyToBuffer(host_preferred_game_id, fmt::format("{:016X}", title_id));
}

void Multiplayer::ShowError(std::string message) {
    LOG_ERROR(Network, "{}", message);
    error_popup = std::move(message);
}

void Multiplayer::Draw() {
    if (shutdown) {
        return;
    }
    BindNetworkCallbacks();
    PollAsyncOperations();
    DrainNetworkEvents();

    DrawLobby();
    DrawHostRoom();
    DrawDirectConnect();
    DrawClientRoom();
    DrawModeration();

    if (!error_popup.empty()) {
        ImGui::OpenPopup("Multiplayer Error");
    }
    if (ImGui::BeginPopupModal("Multiplayer Error", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("%s", error_popup.c_str());
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(160.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            error_popup.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

void Multiplayer::DrawLobby() {
    if (!show_lobby) {
        return;
    }
    SetNextWindowFullArea();
    if (ImGui::Begin("Public Room Browser", nullptr)) {
        HandleGamepadBack(show_lobby);
        GetVirtualKeyboard().InputText("Nickname", lobby_nickname.data(), lobby_nickname.size());

        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        std::array<char, 128> buffer{};
        CopyToBuffer(buffer, lobby_filter);
        GetVirtualKeyboard().InputText("Search", buffer.data(), buffer.size(), "Search...");
        lobby_filter = buffer.data();
        ImGui::SameLine();
        ImGui::Checkbox("Hide full rooms", &lobby_hide_full);
        ImGui::SameLine();
        ImGui::Checkbox("Hide empty rooms", &lobby_hide_empty);
        ImGui::SameLine();
        ImGui::BeginDisabled(lobby_refreshing);
        if (ImGui::Button(lobby_refreshing ? "Refreshing..." : "Refresh")) {
            RefreshLobby();
        }
        ImGui::EndDisabled();

        ImGui::Separator();

        constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_ScrollY |
                                          ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##lobby", 5, flags, ImVec2(0.0f, -140.0f))) {
            ImGui::TableSetupColumn("Room Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Preferred Application", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Host", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 10.0f);
            ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 5.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 4.0f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(lobby_rooms.size()); ++i) {
                const auto& room = lobby_rooms[i];
                if (lobby_hide_full && room.members >= room.max_members) {
                    continue;
                }
                if (lobby_hide_empty && room.members == 0) {
                    continue;
                }
                if (!ContainsInsensitive(room.name, lobby_filter) &&
                    !ContainsInsensitive(room.preferred_game, lobby_filter) &&
                    !ContainsInsensitive(room.owner, lobby_filter)) {
                    continue;
                }

                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(room.name.c_str(), lobby_selected == i,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    lobby_selected = i;
                    lobby_password.fill('\0');
                }
                if (!room.description.empty() && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", room.description.c_str());
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(room.preferred_game.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(room.owner.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%d/%d", room.members, room.max_members);
                ImGui::TableSetColumnIndex(4);
                if (room.has_password) {
                    ImGui::TextDisabled("locked");
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (lobby_rooms.empty() && !lobby_refreshing) {
            ImGui::TextDisabled("No public rooms were returned by the announce service.");
        }

        if (lobby_selected >= 0 && lobby_selected < static_cast<int>(lobby_rooms.size()) &&
            lobby_rooms[lobby_selected].has_password) {
            GetVirtualKeyboard().InputText("Room Password", lobby_password.data(),
                                           lobby_password.size(), nullptr, true);
        }
        ImGui::TextDisabled("Status: %s", connection_status.c_str());

        const bool can_join = lobby_selected >= 0 &&
                              lobby_selected < static_cast<int>(lobby_rooms.size()) &&
                              lobby_nickname[0] != '\0' && !connection_in_progress;
        ImGui::BeginDisabled(!can_join);
        if (ImGui::Button(connection_in_progress ? "Connecting..." : "Join",
                          ImVec2(140.0f, 0.0f))) {
            const auto& room = lobby_rooms[lobby_selected];
            StartJoin(lobby_nickname.data(), room.host, room.port, lobby_password.data(),
                      room.verify_uid);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(140.0f, 0.0f))) {
            show_lobby = false;
        }
    }
    ImGui::End();
}

void Multiplayer::DrawHostRoom() {
    if (!show_host_room) {
        return;
    }
    SetNextWindowFullArea();
    if (ImGui::Begin("Create Room", nullptr)) {
        HandleGamepadBack(show_host_room);
        GetVirtualKeyboard().InputText("Room Name", host_room_name.data(), host_room_name.size());
        GetVirtualKeyboard().InputText("Room Description", host_description.data(),
                                       host_description.size());
        GetVirtualKeyboard().InputText("Nickname", host_nickname.data(), host_nickname.size());
        GetVirtualKeyboard().InputText("Password", host_password.data(), host_password.size(),
                                       nullptr, true);
        Tooltip("Leave empty for an open room.");

        ImGui::InputInt("Port", &host_port);
        host_port = std::clamp(host_port, 1024, 65535);
        ImGui::SliderInt("Max Players", &host_max_members, 2, 16);

        static constexpr std::array kVisibilityItems{"Public", "Unlisted"};
        Combo("Room Visibility", host_visibility, kVisibilityItems,
              "Public rooms are announced to the room browser. Unlisted rooms are reachable by "
              "address only.");

        GetVirtualKeyboard().InputText("Preferred Application", host_preferred_game.data(),
                                       host_preferred_game.size(), "%none% for no preference");
        GetVirtualKeyboard().InputText("Preferred Title ID (hex)", host_preferred_game_id.data(),
                                       host_preferred_game_id.size(), "0004000000000000");
        if (ImGui::Button("Use Running Application")) {
            UseRunningApplicationAsPreference();
        }
        ImGui::SameLine();
        if (ImGui::Button("No Preference")) {
            CopyToBuffer(host_preferred_game, "%none%");
            host_preferred_game_id.fill('\0');
        }
        ImGui::Checkbox("Reuse bans from the previous hosted room", &host_reuse_bans);

        ImGui::Separator();
        ImGui::TextDisabled("Status: %s", connection_status.c_str());
        const bool can_host = host_room_name[0] != '\0' && host_nickname[0] != '\0' &&
                              !connection_in_progress && !host_in_progress;
        ImGui::BeginDisabled(!can_host);
        if (ImGui::Button(host_in_progress ? "Hosting..." : "Host Room", ImVec2(140.0f, 0.0f))) {
            StartHosting();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(140.0f, 0.0f))) {
            show_host_room = false;
        }
    }
    ImGui::End();
}

void Multiplayer::DrawDirectConnect() {
    if (!show_direct_connect) {
        return;
    }
    SetNextWindowFullArea();
    if (ImGui::Begin("Direct Connect to Room", nullptr)) {
        HandleGamepadBack(show_direct_connect);
        GetVirtualKeyboard().InputText("IP Address", direct_address.data(), direct_address.size());
        ImGui::InputInt("Port", &direct_port);
        direct_port = std::clamp(direct_port, 1024, 65535);
        GetVirtualKeyboard().InputText("Nickname", direct_nickname.data(), direct_nickname.size());
        GetVirtualKeyboard().InputText("Password", direct_password.data(), direct_password.size(),
                                       nullptr, true);

        ImGui::Separator();
        ImGui::TextDisabled("Status: %s", connection_status.c_str());
        ImGui::Separator();

        const bool can_connect =
            direct_address[0] != '\0' && direct_nickname[0] != '\0' && !connection_in_progress;
        ImGui::BeginDisabled(!can_connect);
        if (ImGui::Button(connection_in_progress ? "Connecting..." : "Connect",
                          ImVec2(140.0f, 0.0f))) {
            StartJoin(direct_nickname.data(), direct_address.data(), static_cast<u16>(direct_port),
                      direct_password.data(), "");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(140.0f, 0.0f))) {
            show_direct_connect = false;
        }
    }
    ImGui::End();
}

void Multiplayer::DrawClientRoom() {
    if (!show_client_room) {
        return;
    }
    SetNextWindowFullArea();
    if (ImGui::Begin(hosting ? "Room (hosting)" : "Room", nullptr)) {
        HandleGamepadBack(show_client_room);
        ImGui::TextDisabled("Status: %s", StateText(CurrentState()));
        ImGui::Separator();

        const float member_width = ImGui::GetContentRegionAvail().x * 0.32f;
        if (ImGui::BeginChild("##members", ImVec2(member_width, 0.0f), ImGuiChildFlags_Borders)) {
            ImGui::TextDisabled("Members");
            ImGui::Separator();
            std::string self;
            if (const auto member = Network::GetRoomMember().lock()) {
                self = member->GetNickname();
            }
            const bool is_moderator = CurrentState() == Network::RoomMember::State::Moderator;
            for (std::size_t i = 0; i < member_list.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                const bool can_moderate = is_moderator && member_list[i].nickname != self;
                if (can_moderate) {
                    if (ImGui::Selectable(member_list[i].nickname.c_str(),
                                          selected_member == member_list[i].nickname)) {
                        selected_member = member_list[i].nickname;
                    }
                } else {
                    ImGui::TextUnformatted(member_list[i].nickname.c_str());
                }
                if (!member_list[i].game_info.name.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", member_list[i].game_info.name.c_str());
                }
                ImGui::PopID();
            }
            if (is_moderator) {
                const auto selected =
                    std::find_if(member_list.begin(), member_list.end(), [&](const auto& entry) {
                        return entry.nickname == selected_member && entry.nickname != self;
                    });
                const bool has_target = selected != member_list.end();
                ImGui::Separator();
                ImGui::TextDisabled("%s", has_target ? selected_member.c_str()
                                                     : "Select a member to moderate");
                ImGui::BeginDisabled(!has_target);
                if (ImGui::Button("Kick Selected", ImVec2(130.0f, 0.0f))) {
                    if (const auto member = Network::GetRoomMember().lock()) {
                        member->SendModerationRequest(Network::IdModKick, selected_member);
                    }
                    selected_member.clear();
                }
                ImGui::SameLine();
                if (ImGui::Button("Ban Selected", ImVec2(130.0f, 0.0f))) {
                    if (const auto member = Network::GetRoomMember().lock()) {
                        member->SendModerationRequest(Network::IdModBan, selected_member);
                    }
                    selected_member.clear();
                }
                ImGui::EndDisabled();
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();
        if (ImGui::BeginChild("##chat", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
            DrawChat();
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (CurrentState() == Network::RoomMember::State::Moderator) {
            if (ImGui::Button("Moderation...", ImVec2(150.0f, 0.0f))) {
                OpenModeration();
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Leave Room", ImVec2(150.0f, 0.0f))) {
            LeaveRoom();
        }
    }
    ImGui::End();
}

void Multiplayer::DrawChat() {
    const float input_height = ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginChild("##chat_log", ImVec2(0.0f, -input_height))) {
        for (const auto& line : chat_log) {
            ImGui::TextWrapped("%s", line.c_str());
        }
        if (chat_scroll_to_bottom) {
            ImGui::SetScrollHereY(1.0f);
            chat_scroll_to_bottom = false;
        }
    }
    ImGui::EndChild();

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##chat_input", chat_input.data(), chat_input.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (chat_input[0] != '\0' && IsInRoom()) {
            if (const auto member = Network::GetRoomMember().lock()) {
                const std::string message = chat_input.data();
                member->SendChatMessage(message);
                chat_log.emplace_back(fmt::format("{}: {}", member->GetNickname(), message));
            }
            chat_input[0] = '\0';
            chat_scroll_to_bottom = true;
        }
        ImGui::SetKeyboardFocusHere(-1);
    }
}

void Multiplayer::DrawModeration() {
    if (!show_moderation) {
        return;
    }
    SetNextWindowFullArea();
    if (ImGui::Begin("Moderation", nullptr)) {
        HandleGamepadBack(show_moderation);
        ImGui::TextDisabled("Banned accounts and IP addresses");
        ImGui::Separator();

        if (ImGui::BeginChild("##bans", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()))) {
            for (int i = 0; i < static_cast<int>(ban_list.size()); ++i) {
                const std::string label =
                    fmt::format("{} ({})", ban_list[i].subject, ban_list[i].type);
                if (ImGui::Selectable(label.c_str(), moderation_selected == i)) {
                    moderation_selected = i;
                }
            }
            if (ban_list.empty()) {
                ImGui::TextDisabled("No bans.");
            }
        }
        ImGui::EndChild();

        if (ImGui::Button("Refresh", ImVec2(120.0f, 0.0f))) {
            if (const auto member = Network::GetRoomMember().lock()) {
                member->RequestBanList();
            }
        }
        ImGui::SameLine();
        const bool can_unban =
            moderation_selected >= 0 && moderation_selected < static_cast<int>(ban_list.size());
        ImGui::BeginDisabled(!can_unban);
        if (ImGui::Button("Unban", ImVec2(120.0f, 0.0f))) {
            if (const auto member = Network::GetRoomMember().lock()) {
                member->SendModerationRequest(Network::IdModUnban,
                                              ban_list[moderation_selected].subject);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
            show_moderation = false;
        }
    }
    ImGui::End();
}

} // namespace UWP
