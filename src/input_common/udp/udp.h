// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "input_common/udp/client.h"

namespace Common {
class ParamPackage;
}

namespace InputCommon::Polling {
class DevicePoller;
enum class DeviceType;
} // namespace InputCommon::Polling

namespace InputCommon::CemuhookUDP {

struct RemoteLibraryEntry {
    u64 token = 0;
    u64 program_id = 0;
    u64 icon_key = 0;
    u8 flags = 0;
    std::string title;
    std::string long_title;
    std::string file_type;
    std::string region;
};

struct RemoteLibrarySnapshot {
    u32 revision = 0;
    std::vector<RemoteLibraryEntry> entries;
};

struct RemoteIcon {
    u64 key = 0;
    u16 width = 0;
    u16 height = 0;
    std::vector<u16> rgb565;
};

enum class RemoteLaunchStatus : u32 {
    Accepted = 0,
    StaleRevision = 1,
    UnknownEntry = 2,
    PermissionDenied = 3,
    Busy = 4,
};

struct RemoteStreamCommand {
    bool start = false;
    bool auxiliary = false;
    bool audio_enabled = true;
    u8 screen_id = 0;
    u32 stream_type = 4;
    std::string url;
    u32 bitrate_kbps = 4000;
    u16 width = 0;
    u16 height = 0;
    u8 fps = 30;
    u32 authorization_token = 0;
};

struct RemoteFrontendCallbacks {
    std::function<RemoteLibrarySnapshot()> get_library;
    std::function<std::optional<RemoteIcon>(u32 revision, u64 icon_key)> get_icon;
    std::function<RemoteLaunchStatus(u32 revision, u64 token)> launch;
    std::function<u32()> get_extra_capabilities;
    std::function<void(RemoteStreamCommand)> stream_command;
    /// The frontend asked to highlight a catalogue entry without launching it.
    std::function<void(u32 revision, u64 token)> select;
};

struct RemoteFrontendState {
    u8 flags = 0;
    std::string game_name;
    std::string platform_name;
    std::string core_name;
};

struct ExtendedResponse {
    u32 type = 0;
    std::vector<u8> payload;
};

struct RemoteMouseState {
    s32 delta_x = 0;
    s32 delta_y = 0;
    s32 x = 0;
    s32 y = 0;
    u16 buttons = 0;
    u64 sequence = 0;
    bool active = false;
};

struct RemoteKeyboardState {
    std::array<u32, 11> bitmap{};
    u64 sequence = 0;
    bool active = false;
};

void SetRemoteFrontendCallbacks(RemoteFrontendCallbacks callbacks);
void SetRemoteFrontendState(RemoteFrontendState state);
RemoteFrontendState GetRemoteFrontendState();
std::vector<ExtendedResponse> HandleExtendedRequest(u32 type, const u8* payload,
                                                    std::size_t payload_size);
RemoteMouseState GetRemoteMouseState();
RemoteKeyboardState GetRemoteKeyboardState();
u32 GetRemoteFrontendExtraCapabilities();
void QueueRemoteStreamStatus(u32 state, u32 error_code, u32 stream_type, u8 screen_id,
                             const std::string& url, u16 width = 0, u16 height = 0);
void QueueRemoteStreamStatus(u32 state, u32 error_code, u32 stream_type, u8 screen_id,
                             const std::string& url, u16 width, u16 height, bool audio_enabled);
void QueueRemoteBroadcastControlRequest(bool start, u8 stream_mask, u32 authorization_token);
/// Reports the highlighted catalogue entry so the frontend can follow it.
void QueueRemoteAppSelection(u32 revision, u64 token);
std::vector<ExtendedResponse> TakePendingExtendedResponses();

/// DSU 1001 digital-button bit indices. These values are stable in serialized input bindings.
enum class Button : u8 {
    Share,
    L3,
    R3,
    Options,
    Up,
    Right,
    Down,
    Left,
    L2,
    R2,
    L1,
    R1,
    X,
    A,
    B,
    Y,
    Home,
    Touch,
    Count,
};

/// DSU 1001 stick-axis indices. Axis pairs 0/1 and 2/3 are the left and right sticks.
enum class Axis : u8 {
    LeftX,
    LeftY,
    RightX,
    RightY,
    Count,
};

/// Generates stable serialized-binding parameters for DSU gameplay controls.
Common::ParamPackage GenerateButtonParam(Button button, float threshold = 0.5f);
Common::ParamPackage GenerateAnalogParam(Axis axis_x, Axis axis_y, float deadzone = 0.1f);
Common::ParamPackage GenerateAddonButtonParam(const std::string& local_binding, Button button,
                                              float threshold = 0.5f);
Common::ParamPackage GenerateAddonAnalogParam(const std::string& local_binding, Axis axis_x,
                                              Axis axis_y, float deadzone = 0.1f);

class State {
public:
    using Pollers = std::vector<std::unique_ptr<Polling::DevicePoller>>;

    State();
    ~State();
    void ReloadUDPClient();
    Pollers GetPollers(Polling::DeviceType type);

private:
    std::shared_ptr<DeviceStatus> status;
    std::unique_ptr<Client> client;
};

std::unique_ptr<State> Init();

} // namespace InputCommon::CemuhookUDP
