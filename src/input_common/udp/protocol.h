// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <array>
#include <optional>
#include <type_traits>
#include <boost/crc.hpp>
#include "common/swap.h"

namespace InputCommon::CemuhookUDP {

constexpr std::size_t MAX_PACKET_SIZE = 100;
constexpr std::size_t EXTENDED_MAX_PACKET_SIZE = 8192;
constexpr u16 PROTOCOL_VERSION = 1001;
constexpr u32 CLIENT_MAGIC = 0x43555344; // DSUC (but flipped for LE)
constexpr u32 SERVER_MAGIC = 0x53555344; // DSUS (but flipped for LE)

enum class Type : u32 {
    Version = 0x00100000,
    PortInfo = 0x00100001,
    PadData = 0x00100002,
    Keyboard = 0x00120000,
    Mouse = 0x00120001,
    Command = 0x00130000,
    State = 0x00140000,
    StreamStart = 0x00150000,
    StreamStop = 0x00150001,
    StreamStatus = 0x00150002,
    AuxStreamStart = 0x00150100,
    AuxStreamStop = 0x00150101,
    AppLibraryRequest = 0x00180000,
    AppLibraryPage = 0x00180001,
    AppIconRequest = 0x00180002,
    AppIconChunk = 0x00180003,
    AppLaunch = 0x00180004,
    AppLaunchStatus = 0x00180005,
    AppSelection = 0x00180006,
};

struct Header {
    u32_le magic{};
    u16_le protocol_version{};
    u16_le payload_length{};
    u32_le crc{};
    u32_le id{};
    ///> In the protocol, the type of the packet is not part of the header, but its convenient to
    ///> include in the header so the callee doesn't have to duplicate the type twice when building
    ///> the data
    Type type{};
};
static_assert(sizeof(Header) == 20, "UDP Message Header struct has wrong size");
static_assert(std::is_trivially_copyable_v<Header>, "UDP Message Header is not trivially copyable");

using MacAddress = std::array<u8, 6>;
constexpr MacAddress EMPTY_MAC_ADDRESS = {0, 0, 0, 0, 0, 0};

#pragma pack(push, 1)
template <typename T>
struct Message {
    Header header{};
    T data;
};
#pragma pack(pop)

template <typename T>
constexpr Type GetMessageType();

namespace Request {

struct Version {};
/**
 * Requests the server to send information about what controllers are plugged into the ports
 * In citra's case, we only have one controller, so for simplicity's sake, we can just send a
 * request explicitly for the first controller port and leave it at that. In the future it would be
 * nice to make this configurable
 */
constexpr u32 MAX_PORTS = 4;
struct PortInfo {
    u32_le pad_count{}; ///> Number of ports to request data for
    std::array<u8, MAX_PORTS> port;
};
static_assert(std::is_trivially_copyable_v<PortInfo>,
              "UDP Request PortInfo is not trivially copyable");

/**
 * Request the latest pad information from the server. If the server hasn't received this message
 * from the client in a reasonable time frame, the server will stop sending updates. The default
 * timeout seems to be 5 seconds.
 */
struct PadData {
    enum class Flags : u8 {
        AllPorts,
        Id,
        Mac,
    };
    /// Determines which method will be used as a look up for the controller
    Flags flags{};
    /// Index of the port of the controller to retrieve data about
    u8 port_id{};
    /// Mac address of the controller to retrieve data about
    MacAddress mac;
};
static_assert(sizeof(PadData) == 8, "UDP Request PadData struct has wrong size");
static_assert(std::is_trivially_copyable_v<PadData>,
              "UDP Request PadData is not trivially copyable");

/**
 * Creates a message with the proper header data that can be sent to the server.
 * @param T data Request body to send
 * @param client_id ID of the udp client (usually not checked on the server)
 */
template <typename T>
Message<T> Create(const T data, const u32 client_id = 0) {
    boost::crc_32_type crc;
    Header header{
        CLIENT_MAGIC, PROTOCOL_VERSION, sizeof(T) + sizeof(Type), 0, client_id, GetMessageType<T>(),
    };
    Message<T> message{header, data};
    crc.process_bytes(&message, sizeof(Message<T>));
    message.header.crc = crc.checksum();
    return message;
}
} // namespace Request

namespace Response {

struct Version {
    u16_le version{};
};
static_assert(sizeof(Version) == 2, "UDP Response Version struct has wrong size");
static_assert(std::is_trivially_copyable_v<Version>,
              "UDP Response Version is not trivially copyable");

struct PortInfo {
    u8 id{};
    u8 state{};
    u8 model{};
    u8 connection_type{};
    MacAddress mac;
    u8 battery{};
    u8 is_pad_active{};
};
static_assert(sizeof(PortInfo) == 12, "UDP Response PortInfo struct has wrong size");
static_assert(std::is_trivially_copyable_v<PortInfo>,
              "UDP Response PortInfo is not trivially copyable");

#pragma pack(push, 1)
struct PadData {
    PortInfo info{};
    u32_le packet_counter{};

    // Bits 0..15 are Share, L3, R3, Options, Up, Right, Down, Left, L2, R2, L1,
    // R1, X, A, B, and Y respectively.
    u16_le digital_button{};

    u8 home{};
    /// If the device supports a "click" on the touchpad, this will change to 1 when a click happens
    u8 touch_hard_press{};
    u8 left_stick_x{};
    u8 left_stick_y{};
    u8 right_stick_x{};
    u8 right_stick_y{};

    struct AnalogButton {
        // DSU 1001 wire order.
        std::array<u8, 4> dpad{};     // Left, Down, Right, Up
        std::array<u8, 4> face{};     // Y, B, A, X
        std::array<u8, 4> shoulder{}; // R1, L1, R2, L2
    } analog_button;

    struct TouchPad {
        u8 is_active{};
        u8 id{};
        u16_le x{};
        u16_le y{};
    } touch_1, touch_2;

    u64_le motion_timestamp{};

    struct Accelerometer {
        float x{};
        float y{};
        float z{};
    } accel;

    struct Gyroscope {
        float pitch{};
        float yaw{};
        float roll{};
    } gyro;
};
#pragma pack(pop)

static_assert(sizeof(PadData) == 80, "UDP Response PadData struct has wrong size ");
static_assert(std::is_trivially_copyable_v<PadData>,
              "UDP Response PadData is not trivially copyable");

static_assert(sizeof(Message<PadData>) == MAX_PACKET_SIZE,
              "UDP MAX_PACKET_SIZE is no longer larger than Message<PadData>");

static_assert(sizeof(PadData::AnalogButton) == 12,
              "UDP Response AnalogButton struct has wrong size ");
static_assert(sizeof(PadData::TouchPad) == 6, "UDP Response TouchPad struct has wrong size ");
static_assert(sizeof(PadData::Accelerometer) == 12,
              "UDP Response Accelerometer struct has wrong size ");
static_assert(sizeof(PadData::Gyroscope) == 12, "UDP Response Gyroscope struct has wrong size ");

/**
 * Create a Response Message from the data
 * @param data array of bytes sent from the server
 * @return boost::none if it failed to parse or Type if it succeeded. The client can then safely
 * copy the data into the appropriate struct for that Type
 */
std::optional<Type> Validate(u8* data, std::size_t size);

} // namespace Response

template <>
constexpr Type GetMessageType<Request::Version>() {
    return Type::Version;
}
template <>
constexpr Type GetMessageType<Request::PortInfo>() {
    return Type::PortInfo;
}
template <>
constexpr Type GetMessageType<Request::PadData>() {
    return Type::PadData;
}
template <>
constexpr Type GetMessageType<Response::Version>() {
    return Type::Version;
}
template <>
constexpr Type GetMessageType<Response::PortInfo>() {
    return Type::PortInfo;
}
template <>
constexpr Type GetMessageType<Response::PadData>() {
    return Type::PadData;
}
} // namespace InputCommon::CemuhookUDP
