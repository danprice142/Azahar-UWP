// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <tuple>
#include <utility>
#include <boost/crc.hpp>
#include "common/param_package.h"
#include "common/settings.h"
#include "core/frontend/input.h"
#include "input_common/keyboard.h"
#include "input_common/main.h"
#include "input_common/udp/client.h"
#include "input_common/udp/udp.h"

namespace InputCommon::CemuhookUDP {

namespace {

constexpr u32 MSG_KEYBOARD = 0x00120000;
constexpr u32 MSG_MOUSE = 0x00120001;
constexpr u32 MSG_APP_LIBRARY_REQUEST = 0x00180000;
constexpr u32 MSG_APP_LIBRARY_PAGE = 0x00180001;
constexpr u32 MSG_APP_ICON_REQUEST = 0x00180002;
constexpr u32 MSG_APP_ICON_CHUNK = 0x00180003;
constexpr u32 MSG_APP_LAUNCH = 0x00180004;
constexpr u32 MSG_APP_LAUNCH_STATUS = 0x00180005;
constexpr u32 MSG_APP_SELECTION = 0x00180006;
constexpr u32 MSG_BROADCAST_CONTROL_REQUEST = 0x00150003;
constexpr std::size_t LIBRARY_ENTRY_SIZE = 361;
constexpr std::size_t ICON_CHUNK_DATA_SIZE = 1024;

std::mutex remote_frontend_mutex;
RemoteFrontendCallbacks remote_frontend_callbacks;
RemoteFrontendState remote_frontend_state;
std::array<u32, 11> remote_keyboard_bitmap{};
u64 remote_keyboard_sequence = 0;
RemoteMouseState remote_mouse;
std::chrono::steady_clock::time_point remote_keyboard_updated{};
std::chrono::steady_clock::time_point remote_mouse_updated{};
std::vector<ExtendedResponse> pending_extended_responses;

u16 ReadU16(const u8* data) {
    return static_cast<u16>(data[0] | (static_cast<u16>(data[1]) << 8));
}

u32 ReadU32(const u8* data) {
    return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8) |
           (static_cast<u32>(data[2]) << 16) | (static_cast<u32>(data[3]) << 24);
}

u64 ReadU64(const u8* data) {
    return static_cast<u64>(ReadU32(data)) | (static_cast<u64>(ReadU32(data + 4)) << 32);
}

void WriteU16(std::vector<u8>& data, std::size_t offset, u16 value) {
    data[offset] = static_cast<u8>(value);
    data[offset + 1] = static_cast<u8>(value >> 8);
}

void WriteU32(std::vector<u8>& data, std::size_t offset, u32 value) {
    for (int i = 0; i < 4; ++i) {
        data[offset + i] = static_cast<u8>(value >> (i * 8));
    }
}

void WriteU64(std::vector<u8>& data, std::size_t offset, u64 value) {
    WriteU32(data, offset, static_cast<u32>(value));
    WriteU32(data, offset + 4, static_cast<u32>(value >> 32));
}

void WriteString(std::vector<u8>& data, std::size_t offset, std::size_t capacity,
                 const std::string& value) {
    const std::size_t length = std::min(capacity - 1, value.size());
    std::memcpy(data.data() + offset, value.data(), length);
}

ExtendedResponse BuildLaunchStatus(RemoteLaunchStatus status, u32 revision, u64 token) {
    ExtendedResponse response{MSG_APP_LAUNCH_STATUS, std::vector<u8>(144)};
    WriteU32(response.payload, 0, static_cast<u32>(status));
    WriteU32(response.payload, 4, revision);
    WriteU64(response.payload, 8, token);
    const char* message = "Launch accepted";
    switch (status) {
    case RemoteLaunchStatus::StaleRevision:
        message = "Library changed; refresh required";
        break;
    case RemoteLaunchStatus::UnknownEntry:
        message = "Unknown library entry";
        break;
    case RemoteLaunchStatus::PermissionDenied:
        message = "Remote launching is disabled";
        break;
    case RemoteLaunchStatus::Busy:
        message = "Another launch is pending";
        break;
    case RemoteLaunchStatus::Accepted:
        break;
    }
    WriteString(response.payload, 16, 128, message);
    return response;
}

ExtendedResponse BuildStreamStatus(u32 state, u32 error_code, u32 stream_type, u8 screen_id,
                                   const std::string& url, u16 width, u16 height,
                                   bool audio_enabled) {
    // Audio state is appended after the original 280-byte layout so older clients are unaffected.
    ExtendedResponse response{0x00150002, std::vector<u8>(281)};
    WriteU32(response.payload, 0, state);
    WriteU32(response.payload, 4, error_code);
    WriteU32(response.payload, 8, stream_type);
    response.payload[12] = screen_id;
    WriteString(response.payload, 16, 256, url);
    WriteU16(response.payload, 272, width);
    WriteU16(response.payload, 274, height);
    float aspect = height == 0 ? 0.0f : static_cast<float>(width) / height;
    std::memcpy(response.payload.data() + 276, &aspect, sizeof(aspect));
    response.payload[280] = audio_enabled ? 1 : 0;
    return response;
}

constexpr std::array<const char*, static_cast<std::size_t>(Button::Count)> BUTTON_NAMES{
    "Share", "L3", "R3", "Options", "D-Pad Up", "D-Pad Right", "D-Pad Down", "D-Pad Left", "L2",
    "R2",    "L1", "R1", "X",       "A",        "B",           "Y",          "Home",       "Touch",
};

constexpr std::array<const char*, static_cast<std::size_t>(Axis::Count)> AXIS_NAMES{
    "DSU Left X",
    "DSU Left Y",
    "DSU Right X",
    "DSU Right Y",
};

bool IsFreshAndActive(const DeviceStatus& status) {
    if (!status.pad_status.active ||
        status.last_update == std::chrono::steady_clock::time_point{}) {
        return false;
    }
    return std::chrono::steady_clock::now() - status.last_update <= DeviceStatus::PACKET_TIMEOUT;
}

float NormalizeAxis(u8 value) {
    const int offset = static_cast<int>(value) - 128;
    // The two halves of the byte range differ in length; map both poles exactly with 128 at zero.
    return offset >= 0 ? static_cast<float>(offset) / 127.0f : static_cast<float>(offset) / 128.0f;
}

float GetAxis(const DeviceStatus::PadStatus& pad, int axis) {
    if (axis < 0 || axis >= static_cast<int>(Axis::Count)) {
        return 0.0f;
    }
    return NormalizeAxis(pad.stick_axes[static_cast<std::size_t>(axis)]);
}

u8 GetAnalogButton(const DeviceStatus::PadStatus& pad, Button button) {
    switch (button) {
    case Button::Up:
        return pad.analog_dpad[3];
    case Button::Right:
        return pad.analog_dpad[2];
    case Button::Down:
        return pad.analog_dpad[1];
    case Button::Left:
        return pad.analog_dpad[0];
    case Button::L2:
        return pad.analog_shoulder[3];
    case Button::R2:
        return pad.analog_shoulder[2];
    case Button::L1:
        return pad.analog_shoulder[1];
    case Button::R1:
        return pad.analog_shoulder[0];
    case Button::X:
        return pad.analog_face[3];
    case Button::A:
        return pad.analog_face[2];
    case Button::B:
        return pad.analog_face[1];
    case Button::Y:
        return pad.analog_face[0];
    default:
        return 0;
    }
}

bool IsButtonPressed(const DeviceStatus::PadStatus& pad, Button button, float threshold) {
    const auto index = static_cast<u8>(button);
    if (index < 16 && (pad.digital_buttons & (u16{1} << index)) != 0) {
        return true;
    }
    if (button == Button::Home) {
        return pad.home;
    }
    if (button == Button::Touch) {
        return pad.touch_hard_press;
    }
    return static_cast<float>(GetAnalogButton(pad, button)) / 255.0f > threshold;
}

struct PadSnapshot {
    bool active{};
    DeviceStatus::PadStatus pad;
};

PadSnapshot ReadPad(const std::shared_ptr<DeviceStatus>& status) {
    std::lock_guard guard(status->update_mutex);
    return {IsFreshAndActive(*status), status->pad_status};
}

} // namespace

void SetRemoteFrontendCallbacks(RemoteFrontendCallbacks callbacks) {
    std::lock_guard lock{remote_frontend_mutex};
    remote_frontend_callbacks = std::move(callbacks);
    if (!remote_frontend_callbacks.get_library) {
        remote_keyboard_bitmap.fill(0);
        ++remote_keyboard_sequence;
        remote_keyboard_updated = {};
        remote_mouse = {};
        remote_mouse_updated = {};
    }
}

void SetRemoteFrontendState(RemoteFrontendState state) {
    std::lock_guard lock{remote_frontend_mutex};
    remote_frontend_state = std::move(state);
}

RemoteFrontendState GetRemoteFrontendState() {
    std::lock_guard lock{remote_frontend_mutex};
    return remote_frontend_state;
}

RemoteMouseState GetRemoteMouseState() {
    std::lock_guard lock{remote_frontend_mutex};
    auto result = remote_mouse;
    result.active =
        remote_mouse_updated != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() - remote_mouse_updated < std::chrono::milliseconds{1500};
    return result;
}

RemoteKeyboardState GetRemoteKeyboardState() {
    std::lock_guard lock{remote_frontend_mutex};
    RemoteKeyboardState result;
    result.bitmap = remote_keyboard_bitmap;
    result.sequence = remote_keyboard_sequence;
    result.active = remote_keyboard_updated != std::chrono::steady_clock::time_point{};
    return result;
}

u32 GetRemoteFrontendExtraCapabilities() {
    std::function<u32()> callback;
    {
        std::lock_guard lock{remote_frontend_mutex};
        callback = remote_frontend_callbacks.get_extra_capabilities;
    }
    return callback ? callback() : 0;
}

void QueueRemoteStreamStatus(u32 state, u32 error_code, u32 stream_type, u8 screen_id,
                             const std::string& url, u16 width, u16 height) {
    // Callers that do not report audio state keep the main/aux defaults.
    QueueRemoteStreamStatus(state, error_code, stream_type, screen_id, url, width, height,
                            screen_id == 0);
}

void QueueRemoteStreamStatus(u32 state, u32 error_code, u32 stream_type, u8 screen_id,
                             const std::string& url, u16 width, u16 height, bool audio_enabled) {
    std::lock_guard lock{remote_frontend_mutex};
    pending_extended_responses.emplace_back(BuildStreamStatus(
        state, error_code, stream_type, screen_id, url, width, height, audio_enabled));
}

void QueueRemoteBroadcastControlRequest(bool start, u8 stream_mask, u32 authorization_token) {
    ExtendedResponse response{MSG_BROADCAST_CONTROL_REQUEST, std::vector<u8>(6)};
    response.payload[0] = start ? 1 : 0;
    response.payload[1] = stream_mask;
    WriteU32(response.payload, 2, authorization_token);

    std::lock_guard lock{remote_frontend_mutex};
    pending_extended_responses.emplace_back(std::move(response));
}

void QueueRemoteAppSelection(u32 revision, u64 token) {
    ExtendedResponse response{MSG_APP_SELECTION, std::vector<u8>(12)};
    WriteU32(response.payload, 0, revision);
    WriteU64(response.payload, 4, token);

    std::lock_guard lock{remote_frontend_mutex};
    pending_extended_responses.emplace_back(std::move(response));
}

std::vector<ExtendedResponse> TakePendingExtendedResponses() {
    std::lock_guard lock{remote_frontend_mutex};
    return std::exchange(pending_extended_responses, {});
}

std::vector<ExtendedResponse> HandleExtendedRequest(u32 type, const u8* payload,
                                                    std::size_t payload_size) {
    RemoteFrontendCallbacks callbacks;
    {
        std::lock_guard lock{remote_frontend_mutex};
        callbacks = remote_frontend_callbacks;
    }

    if (type == MSG_KEYBOARD && payload_size >= 51) {
        auto* keyboard = InputCommon::GetKeyboard();
        if (!keyboard) {
            return {};
        }
        std::lock_guard lock{remote_frontend_mutex};
        for (std::size_t word = 0; word < remote_keyboard_bitmap.size(); ++word) {
            const u32 next = ReadU32(payload + 7 + word * 4);
            const u32 changed = next ^ remote_keyboard_bitmap[word];
            for (u32 bit = 0; bit < 32; ++bit) {
                if ((changed & (u32{1} << bit)) == 0) {
                    continue;
                }
                const int key = static_cast<int>(word * 32 + bit);
                if ((next & (u32{1} << bit)) != 0) {
                    keyboard->PressKey(key);
                } else {
                    keyboard->ReleaseKey(key);
                }
            }
            remote_keyboard_bitmap[word] = next;
        }
        ++remote_keyboard_sequence;
        remote_keyboard_updated = std::chrono::steady_clock::now();
        return {};
    }

    if (type == MSG_MOUSE && payload_size >= 15) {
        const auto read_s16 = [](const u8* data) { return static_cast<s16>(ReadU16(data)); };
        std::lock_guard lock{remote_frontend_mutex};
        remote_mouse.delta_x = read_s16(payload + 1);
        remote_mouse.delta_y = read_s16(payload + 3);
        remote_mouse.x = read_s16(payload + 9);
        remote_mouse.y = read_s16(payload + 11);
        remote_mouse.buttons = ReadU16(payload + 13);
        ++remote_mouse.sequence;
        remote_mouse_updated = std::chrono::steady_clock::now();
        return {};
    }

    if (type == MSG_APP_LIBRARY_REQUEST) {
        if (!callbacks.get_library) {
            return {};
        }
        const RemoteLibrarySnapshot snapshot = callbacks.get_library();
        const u16 requested_page = payload_size >= 6 ? ReadU16(payload + 4) : 0;
        const u16 requested_size = payload_size >= 8 ? ReadU16(payload + 6) : 3;
        const u16 page_size = std::clamp<u16>(requested_size, 1, 3);
        const std::size_t first = static_cast<std::size_t>(requested_page) * page_size;
        const std::size_t count =
            first < snapshot.entries.size()
                ? std::min<std::size_t>(page_size, snapshot.entries.size() - first)
                : 0;
        const u16 total_pages = static_cast<u16>(
            std::max<std::size_t>(1, (snapshot.entries.size() + page_size - 1) / page_size));
        ExtendedResponse response{MSG_APP_LIBRARY_PAGE,
                                  std::vector<u8>(10 + count * LIBRARY_ENTRY_SIZE)};
        WriteU32(response.payload, 0, snapshot.revision);
        WriteU16(response.payload, 4, requested_page);
        WriteU16(response.payload, 6, total_pages);
        WriteU16(response.payload, 8, static_cast<u16>(count));
        for (std::size_t index = 0; index < count; ++index) {
            const auto& entry = snapshot.entries[first + index];
            const std::size_t offset = 10 + index * LIBRARY_ENTRY_SIZE;
            WriteU64(response.payload, offset, entry.token);
            WriteU64(response.payload, offset + 8, entry.program_id);
            WriteU64(response.payload, offset + 16, entry.icon_key);
            response.payload[offset + 24] = entry.flags;
            WriteString(response.payload, offset + 25, 96, entry.title);
            WriteString(response.payload, offset + 121, 160, entry.long_title);
            WriteString(response.payload, offset + 281, 16, entry.file_type);
            WriteString(response.payload, offset + 297, 64, entry.region);
        }
        return {std::move(response)};
    }

    if (type == MSG_APP_ICON_REQUEST && payload_size >= 12 && callbacks.get_icon) {
        const u32 revision = ReadU32(payload);
        const u64 key = ReadU64(payload + 4);
        const auto icon = callbacks.get_icon(revision, key);
        if (!icon || icon->rgb565.empty()) {
            return {};
        }
        const auto* bytes = reinterpret_cast<const u8*>(icon->rgb565.data());
        const std::size_t byte_count = icon->rgb565.size() * sizeof(u16);
        boost::crc_32_type crc;
        crc.process_bytes(bytes, byte_count);
        const u16 chunk_count =
            static_cast<u16>((byte_count + ICON_CHUNK_DATA_SIZE - 1) / ICON_CHUNK_DATA_SIZE);
        std::vector<ExtendedResponse> responses;
        responses.reserve(chunk_count);
        for (u16 chunk = 0; chunk < chunk_count; ++chunk) {
            const std::size_t source_offset = chunk * ICON_CHUNK_DATA_SIZE;
            const std::size_t amount =
                std::min<std::size_t>(ICON_CHUNK_DATA_SIZE, byte_count - source_offset);
            ExtendedResponse response{MSG_APP_ICON_CHUNK, std::vector<u8>(28 + amount)};
            WriteU32(response.payload, 0, revision);
            WriteU64(response.payload, 4, key);
            WriteU16(response.payload, 12, chunk);
            WriteU16(response.payload, 14, chunk_count);
            WriteU16(response.payload, 16, icon->width);
            WriteU16(response.payload, 18, icon->height);
            WriteU32(response.payload, 20, static_cast<u32>(byte_count));
            WriteU32(response.payload, 24, crc.checksum());
            std::memcpy(response.payload.data() + 28, bytes + source_offset, amount);
            responses.emplace_back(std::move(response));
        }
        return responses;
    }

    if (type == MSG_APP_LAUNCH && payload_size >= 12) {
        const u32 revision = ReadU32(payload);
        const u64 token = ReadU64(payload + 4);
        const RemoteLaunchStatus status = callbacks.launch ? callbacks.launch(revision, token)
                                                           : RemoteLaunchStatus::PermissionDenied;
        return {BuildLaunchStatus(status, revision, token)};
    }

    if (type == MSG_APP_SELECTION && payload_size >= 12 && callbacks.select) {
        callbacks.select(ReadU32(payload), ReadU64(payload + 4));
        return {};
    }

    if ((type == 0x00150000 || type == 0x00150100) && callbacks.stream_command) {
        const bool auxiliary = type == 0x00150100;
        const std::size_t base = auxiliary ? 1 : 0;
        if (payload_size < base + 269) {
            return {};
        }
        RemoteStreamCommand command;
        command.start = true;
        command.auxiliary = auxiliary;
        command.screen_id = auxiliary ? payload[0] : 0;
        command.stream_type = ReadU32(payload + base);
        command.url.assign(reinterpret_cast<const char*>(payload + base + 4),
                           strnlen(reinterpret_cast<const char*>(payload + base + 4), 256));
        command.bitrate_kbps = ReadU32(payload + base + 260);
        command.width = ReadU16(payload + base + 264);
        command.height = ReadU16(payload + base + 266);
        command.fps = payload[base + 268];
        // Byte 269 is optional; older clients keep audio on the main stream only.
        command.audio_enabled = payload_size > base + 269 ? payload[base + 269] != 0 : !auxiliary;
        if (payload_size >= base + 274) {
            command.authorization_token = ReadU32(payload + base + 270);
        }
        callbacks.stream_command(std::move(command));
        return {};
    }

    if ((type == 0x00150001 || type == 0x00150101) && callbacks.stream_command) {
        RemoteStreamCommand command;
        command.start = false;
        command.auxiliary = type == 0x00150101;
        command.screen_id = command.auxiliary && payload_size > 0 ? payload[0] : 0;
        callbacks.stream_command(std::move(command));
        return {};
    }
    return {};
}

Common::ParamPackage GenerateButtonParam(Button button, float threshold) {
    const int index = std::clamp(static_cast<int>(button), 0, static_cast<int>(Button::Count) - 1);
    Common::ParamPackage params{{"engine", "cemuhookudp"}};
    params.Set("button", index);
    params.Set("name", std::string{"DSU "} + BUTTON_NAMES[static_cast<std::size_t>(index)]);
    params.Set("threshold", std::clamp(threshold, 0.0f, 1.0f));
    return params;
}

Common::ParamPackage GenerateAnalogParam(Axis axis_x, Axis axis_y, float deadzone) {
    const int x = std::clamp(static_cast<int>(axis_x), 0, static_cast<int>(Axis::Count) - 1);
    const int y = std::clamp(static_cast<int>(axis_y), 0, static_cast<int>(Axis::Count) - 1);
    Common::ParamPackage params{{"engine", "cemuhookudp"}};
    params.Set("axis_x", x);
    params.Set("axis_y", y);
    params.Set("name_x", AXIS_NAMES[static_cast<std::size_t>(x)]);
    params.Set("name_y", AXIS_NAMES[static_cast<std::size_t>(y)]);
    params.Set("deadzone", std::clamp(deadzone, 0.0f, 0.99f));
    return params;
}

Common::ParamPackage GenerateAddonButtonParam(const std::string& local_binding, Button button,
                                              float threshold) {
    Common::ParamPackage params = GenerateButtonParam(button, threshold);
    params.Set("engine", "cemuhookudp_addon");
    params.Set("local", local_binding);
    return params;
}

Common::ParamPackage GenerateAddonAnalogParam(const std::string& local_binding, Axis axis_x,
                                              Axis axis_y, float deadzone) {
    Common::ParamPackage params = GenerateAnalogParam(axis_x, axis_y, deadzone);
    params.Set("engine", "cemuhookudp_addon");
    params.Set("local", local_binding);
    return params;
}

class UDPButtonDevice final : public Input::ButtonDevice {
public:
    UDPButtonDevice(std::shared_ptr<DeviceStatus> status_, Button button_, float threshold_)
        : status(std::move(status_)), button(button_), threshold(threshold_) {}

    UDPButtonDevice(std::shared_ptr<DeviceStatus> status_, Axis axis_, float threshold_,
                    bool trigger_if_greater_)
        : status(std::move(status_)), axis(axis_), threshold(threshold_),
          trigger_if_greater(trigger_if_greater_) {}

    bool GetStatus() const override {
        std::lock_guard guard(status->update_mutex);
        if (!IsFreshAndActive(*status)) {
            return false;
        }
        if (axis) {
            const float value = GetAxis(status->pad_status, static_cast<int>(*axis));
            return trigger_if_greater ? value > threshold : value < threshold;
        }
        return IsButtonPressed(status->pad_status, button, threshold);
    }

private:
    std::shared_ptr<DeviceStatus> status;
    Button button{Button::Share};
    std::optional<Axis> axis;
    float threshold{};
    bool trigger_if_greater{};
};

class UDPAnalogDevice final : public Input::AnalogDevice {
public:
    UDPAnalogDevice(std::shared_ptr<DeviceStatus> status_, Axis axis_x_, Axis axis_y_,
                    float deadzone_, bool invert_x_, bool invert_y_)
        : status(std::move(status_)), axis_x(axis_x_), axis_y(axis_y_), deadzone(deadzone_),
          invert_x(invert_x_), invert_y(invert_y_) {}

    std::tuple<float, float> GetStatus() const override {
        std::lock_guard guard(status->update_mutex);
        if (!IsFreshAndActive(*status)) {
            return {0.0f, 0.0f};
        }

        float x = GetAxis(status->pad_status, static_cast<int>(axis_x));
        float y = GetAxis(status->pad_status, static_cast<int>(axis_y));
        x = invert_x ? -x : x;
        y = invert_y ? -y : y;

        float radius = std::sqrt(x * x + y * y);
        if (radius > 1.0f) {
            x /= radius;
            y /= radius;
            radius = 1.0f;
        }
        if (radius <= deadzone) {
            return {0.0f, 0.0f};
        }
        const float scaled_radius = (radius - deadzone) / (1.0f - deadzone);
        return {x / radius * scaled_radius, y / radius * scaled_radius};
    }

private:
    std::shared_ptr<DeviceStatus> status;
    Axis axis_x;
    Axis axis_y;
    float deadzone{};
    bool invert_x{};
    bool invert_y{};
};

class UDPAddonButtonDevice final : public Input::ButtonDevice {
public:
    UDPAddonButtonDevice(std::unique_ptr<Input::ButtonDevice> local_,
                         std::unique_ptr<Input::ButtonDevice> remote_)
        : local(std::move(local_)), remote(std::move(remote_)) {}

    bool GetStatus() const override {
        return (local && local->GetStatus()) || (remote && remote->GetStatus());
    }

private:
    std::unique_ptr<Input::ButtonDevice> local;
    std::unique_ptr<Input::ButtonDevice> remote;
};

class UDPAddonAnalogDevice final : public Input::AnalogDevice {
public:
    UDPAddonAnalogDevice(std::unique_ptr<Input::AnalogDevice> local_,
                         std::unique_ptr<Input::AnalogDevice> remote_)
        : local(std::move(local_)), remote(std::move(remote_)) {}

    std::tuple<float, float> GetStatus() const override {
        const auto local_value = local ? local->GetStatus() : std::tuple<float, float>{};
        const auto remote_value = remote ? remote->GetStatus() : std::tuple<float, float>{};
        const auto magnitude_squared = [](const auto& value) {
            return std::get<0>(value) * std::get<0>(value) +
                   std::get<1>(value) * std::get<1>(value);
        };
        return magnitude_squared(remote_value) > magnitude_squared(local_value) ? remote_value
                                                                                : local_value;
    }

private:
    std::unique_ptr<Input::AnalogDevice> local;
    std::unique_ptr<Input::AnalogDevice> remote;
};

class UDPTouchDevice final : public Input::TouchDevice {
public:
    explicit UDPTouchDevice(std::shared_ptr<DeviceStatus> status_) : status(std::move(status_)) {}
    std::tuple<float, float, bool> GetStatus() const override {
        std::lock_guard guard(status->update_mutex);
        if (!IsFreshAndActive(*status)) {
            return {0.0f, 0.0f, false};
        }
        return status->touch_status;
    }

private:
    std::shared_ptr<DeviceStatus> status;
};

class UDPMotionDevice final : public Input::MotionDevice {
public:
    explicit UDPMotionDevice(std::shared_ptr<DeviceStatus> status_) : status(std::move(status_)) {}
    std::tuple<Common::Vec3<float>, Common::Vec3<float>> GetStatus() const override {
        std::lock_guard guard(status->update_mutex);
        if (!IsFreshAndActive(*status)) {
            return {Common::Vec3f{}, Common::Vec3f{}};
        }
        return status->motion_status;
    }

private:
    std::shared_ptr<DeviceStatus> status;
};

class UDPButtonFactory final : public Input::Factory<Input::ButtonDevice> {
public:
    explicit UDPButtonFactory(std::shared_ptr<DeviceStatus> status_) : status(std::move(status_)) {}

    /**
     * Creates a DSU button. A binding may use either:
     *   - "button": a Button bit index (0..17), optionally with an analog-pressure threshold
     *   - "axis": a stick Axis index (0..3), "direction" (+/-), and threshold
     */
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override {
        if (params.Has("axis")) {
            const int axis =
                std::clamp(params.Get("axis", 0), 0, static_cast<int>(Axis::Count) - 1);
            const std::string direction = params.Get("direction", "+");
            if (direction != "+" && direction != "-") {
                LOG_ERROR(Input, "Unknown DSU axis direction {}", direction);
            }
            const float default_threshold = direction == "-" ? -0.5f : 0.5f;
            const float threshold =
                std::clamp(params.Get("threshold", default_threshold), -1.0f, 1.0f);
            return std::make_unique<UDPButtonDevice>(status, static_cast<Axis>(axis), threshold,
                                                     direction != "-");
        }

        const float threshold = std::clamp(params.Get("threshold", 0.5f), 0.0f, 1.0f);
        const int button =
            std::clamp(params.Get("button", 0), 0, static_cast<int>(Button::Count) - 1);
        return std::make_unique<UDPButtonDevice>(status, static_cast<Button>(button), threshold);
    }

private:
    std::shared_ptr<DeviceStatus> status;
};

class UDPAnalogFactory final : public Input::Factory<Input::AnalogDevice> {
public:
    explicit UDPAnalogFactory(std::shared_ptr<DeviceStatus> status_) : status(std::move(status_)) {}

    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage& params) override {
        const int axis_x =
            std::clamp(params.Get("axis_x", 0), 0, static_cast<int>(Axis::Count) - 1);
        const int axis_y =
            std::clamp(params.Get("axis_y", 1), 0, static_cast<int>(Axis::Count) - 1);
        const float deadzone = std::clamp(params.Get("deadzone", 0.1f), 0.0f, 0.99f);
        return std::make_unique<UDPAnalogDevice>(
            status, static_cast<Axis>(axis_x), static_cast<Axis>(axis_y), deadzone,
            params.Get("invert_x", false), params.Get("invert_y", false));
    }

private:
    std::shared_ptr<DeviceStatus> status;
};

class UDPAddonButtonFactory final : public Input::Factory<Input::ButtonDevice> {
public:
    explicit UDPAddonButtonFactory(std::shared_ptr<DeviceStatus> status_)
        : remote_factory(std::move(status_)) {}

    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override {
        const std::string local_params = params.Get("local", "");
        auto local =
            local_params.empty() ? nullptr : Input::CreateDevice<Input::ButtonDevice>(local_params);
        Common::ParamPackage remote_params = params;
        remote_params.Set("engine", "cemuhookudp");
        return std::make_unique<UDPAddonButtonDevice>(std::move(local),
                                                      remote_factory.Create(remote_params));
    }

private:
    UDPButtonFactory remote_factory;
};

class UDPAddonAnalogFactory final : public Input::Factory<Input::AnalogDevice> {
public:
    explicit UDPAddonAnalogFactory(std::shared_ptr<DeviceStatus> status_)
        : remote_factory(std::move(status_)) {}

    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage& params) override {
        const std::string local_params = params.Get("local", "");
        auto local =
            local_params.empty() ? nullptr : Input::CreateDevice<Input::AnalogDevice>(local_params);
        Common::ParamPackage remote_params = params;
        remote_params.Set("engine", "cemuhookudp");
        return std::make_unique<UDPAddonAnalogDevice>(std::move(local),
                                                      remote_factory.Create(remote_params));
    }

private:
    UDPAnalogFactory remote_factory;
};

class UDPTouchFactory final : public Input::Factory<Input::TouchDevice> {
public:
    explicit UDPTouchFactory(std::shared_ptr<DeviceStatus> status_) : status(std::move(status_)) {}

    std::unique_ptr<Input::TouchDevice> Create(const Common::ParamPackage& params) override {
        {
            std::lock_guard guard(status->update_mutex);
            status->touch_calibration = DeviceStatus::CalibrationData{};
            // These default values work well for DS4 but probably not other touch inputs
            const int min_x = std::clamp(params.Get("min_x", 100), 0, 65534);
            const int min_y = std::clamp(params.Get("min_y", 50), 0, 65534);
            status->touch_calibration->min_x = static_cast<u16>(min_x);
            status->touch_calibration->min_y = static_cast<u16>(min_y);
            status->touch_calibration->max_x =
                static_cast<u16>(std::clamp(params.Get("max_x", 1800), min_x + 1, 65535));
            status->touch_calibration->max_y =
                static_cast<u16>(std::clamp(params.Get("max_y", 850), min_y + 1, 65535));
        }
        return std::make_unique<UDPTouchDevice>(status);
    }

private:
    std::shared_ptr<DeviceStatus> status;
};

class UDPMotionFactory final : public Input::Factory<Input::MotionDevice> {
public:
    explicit UDPMotionFactory(std::shared_ptr<DeviceStatus> status_) : status(std::move(status_)) {}

    std::unique_ptr<Input::MotionDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<UDPMotionDevice>(status);
    }

private:
    std::shared_ptr<DeviceStatus> status;
};

namespace Polling {

class UDPButtonPoller final : public InputCommon::Polling::DevicePoller {
public:
    explicit UDPButtonPoller(std::shared_ptr<DeviceStatus> status_) : status(std::move(status_)) {}

    void Start() override {
        polling = true;
        const auto snapshot = ReadPad(status);
        for (std::size_t i = 0; i < previous.size(); ++i) {
            previous[i] = snapshot.active &&
                          IsButtonPressed(snapshot.pad, static_cast<Button>(i), /*threshold=*/0.5f);
        }
    }

    void Stop() override {
        polling = false;
        previous.fill(false);
    }

    Common::ParamPackage GetNextInput() override {
        if (!polling) {
            return {};
        }
        const auto snapshot = ReadPad(status);
        std::array<bool, static_cast<std::size_t>(Button::Count)> current{};
        for (std::size_t i = 0; i < current.size(); ++i) {
            current[i] = snapshot.active &&
                         IsButtonPressed(snapshot.pad, static_cast<Button>(i), /*threshold=*/0.5f);
        }
        for (std::size_t i = 0; i < current.size(); ++i) {
            if (current[i] && !previous[i]) {
                previous = current;
                return GenerateButtonParam(static_cast<Button>(i));
            }
        }
        previous = current;
        return {};
    }

private:
    std::shared_ptr<DeviceStatus> status;
    std::array<bool, static_cast<std::size_t>(Button::Count)> previous{};
    bool polling{};
};

class UDPAnalogPoller final : public InputCommon::Polling::DevicePoller {
public:
    explicit UDPAnalogPoller(std::shared_ptr<DeviceStatus> status_) : status(std::move(status_)) {}

    void Start() override {
        polling = true;
        const auto snapshot = ReadPad(status);
        for (int stick = 0; stick < 2; ++stick) {
            ready[stick] = !snapshot.active || Magnitude(snapshot.pad, stick) < 0.25f;
        }
    }

    void Stop() override {
        polling = false;
        ready.fill(false);
    }

    Common::ParamPackage GetNextInput() override {
        if (!polling) {
            return {};
        }
        const auto snapshot = ReadPad(status);
        if (!snapshot.active) {
            ready.fill(true);
            return {};
        }

        for (int stick = 0; stick < 2; ++stick) {
            const float magnitude = Magnitude(snapshot.pad, stick);
            if (magnitude < 0.25f) {
                ready[stick] = true;
            } else if (ready[stick] && magnitude > 0.5f) {
                ready[stick] = false;
                const int axis_x = stick * 2;
                return GenerateAnalogParam(static_cast<Axis>(axis_x),
                                           static_cast<Axis>(axis_x + 1));
            }
        }
        return {};
    }

private:
    static float Magnitude(const DeviceStatus::PadStatus& pad, int stick) {
        const int axis_x = stick * 2;
        const float x = GetAxis(pad, axis_x);
        const float y = GetAxis(pad, axis_x + 1);
        return std::sqrt(x * x + y * y);
    }

    std::shared_ptr<DeviceStatus> status;
    std::array<bool, 2> ready{};
    bool polling{};
};

} // namespace Polling

State::State() : status(std::make_shared<DeviceStatus>()) {
    client =
        std::make_unique<Client>(status, Settings::values.current_input_profile.udp_input_address,
                                 Settings::values.current_input_profile.udp_input_port,
                                 Settings::values.current_input_profile.udp_pad_index);

    Input::RegisterFactory<Input::ButtonDevice>("cemuhookudp",
                                                std::make_shared<UDPButtonFactory>(status));
    Input::RegisterFactory<Input::AnalogDevice>("cemuhookudp",
                                                std::make_shared<UDPAnalogFactory>(status));
    Input::RegisterFactory<Input::ButtonDevice>("cemuhookudp_addon",
                                                std::make_shared<UDPAddonButtonFactory>(status));
    Input::RegisterFactory<Input::AnalogDevice>("cemuhookudp_addon",
                                                std::make_shared<UDPAddonAnalogFactory>(status));
    Input::RegisterFactory<Input::TouchDevice>("cemuhookudp",
                                               std::make_shared<UDPTouchFactory>(status));
    Input::RegisterFactory<Input::MotionDevice>("cemuhookudp",
                                                std::make_shared<UDPMotionFactory>(status));
}

State::~State() {
    Input::UnregisterFactory<Input::ButtonDevice>("cemuhookudp");
    Input::UnregisterFactory<Input::AnalogDevice>("cemuhookudp");
    Input::UnregisterFactory<Input::ButtonDevice>("cemuhookudp_addon");
    Input::UnregisterFactory<Input::AnalogDevice>("cemuhookudp_addon");
    Input::UnregisterFactory<Input::TouchDevice>("cemuhookudp");
    Input::UnregisterFactory<Input::MotionDevice>("cemuhookudp");
}

void State::ReloadUDPClient() {
    client->ReloadSocket(Settings::values.current_input_profile.udp_input_address,
                         Settings::values.current_input_profile.udp_input_port,
                         Settings::values.current_input_profile.udp_pad_index);
}

State::Pollers State::GetPollers(InputCommon::Polling::DeviceType type) {
    Pollers pollers;
    switch (type) {
    case InputCommon::Polling::DeviceType::Button:
        pollers.emplace_back(std::make_unique<Polling::UDPButtonPoller>(status));
        break;
    case InputCommon::Polling::DeviceType::Analog:
        pollers.emplace_back(std::make_unique<Polling::UDPAnalogPoller>(status));
        break;
    case InputCommon::Polling::DeviceType::Touchpad:
        break;
    }
    return pollers;
}

std::unique_ptr<State> Init() {
    return std::make_unique<State>();
}
} // namespace InputCommon::CemuhookUDP
