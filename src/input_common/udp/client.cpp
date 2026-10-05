// Copyright 2018-2025 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <functional>
#include <limits>
#include <thread>
#include <vector>
#include <boost/asio.hpp>
#include "common/logging/log.h"
#include "input_common/udp/client.h"
#include "input_common/udp/protocol.h"
#include "input_common/udp/udp.h"

using boost::asio::ip::udp;

namespace InputCommon::CemuhookUDP {

namespace {

struct ValidPacket {
    u32 type = 0;
    const u8* payload = nullptr;
    std::size_t payload_size = 0;
};

std::optional<ValidPacket> ValidatePacket(const u8* data, std::size_t size) {
    if (size < sizeof(Header)) {
        return std::nullopt;
    }
    Header header{};
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != SERVER_MAGIC || header.protocol_version != PROTOCOL_VERSION ||
        header.payload_length < sizeof(Type)) {
        return std::nullopt;
    }
    const std::size_t packet_size =
        sizeof(Header) + static_cast<std::size_t>(header.payload_length) - sizeof(Type);
    if (packet_size != size) {
        return std::nullopt;
    }
    std::vector<u8> crc_data(data, data + size);
    std::memset(crc_data.data() + offsetof(Header, crc), 0, sizeof(u32_le));
    boost::crc_32_type crc;
    crc.process_bytes(crc_data.data(), crc_data.size());
    if (crc.checksum() != static_cast<u32>(header.crc)) {
        return std::nullopt;
    }
    return ValidPacket{static_cast<u32>(header.type), data + sizeof(Header), size - sizeof(Header)};
}

std::vector<u8> BuildClientPacket(u32 type, const std::vector<u8>& payload, u32 client_id) {
    std::vector<u8> packet(sizeof(Header) + payload.size());
    Header header{
        CLIENT_MAGIC, PROTOCOL_VERSION,       static_cast<u16>(payload.size() + sizeof(Type)), 0,
        client_id,    static_cast<Type>(type)};
    std::memcpy(packet.data(), &header, sizeof(header));
    if (!payload.empty()) {
        std::memcpy(packet.data() + sizeof(header), payload.data(), payload.size());
    }
    boost::crc_32_type crc;
    crc.process_bytes(packet.data(), packet.size());
    const u32 checksum = crc.checksum();
    std::memcpy(packet.data() + offsetof(Header, crc), &checksum, sizeof(checksum));
    return packet;
}

std::vector<u8> BuildAzaharState() {
    constexpr u32 cap_state = 1U << 0;
    constexpr u32 cap_remote_commands = 1U << 6;
    constexpr u32 cap_launch_by_id = 1U << 9;
    std::vector<u8> payload(261);
    const auto state = GetRemoteFrontendState();
    payload[0] = state.flags;
    const auto copy_string = [&payload](std::size_t offset, const std::string& value) {
        const std::size_t length = std::min<std::size_t>(63, value.size());
        std::memcpy(payload.data() + offset, value.data(), length);
    };
    copy_string(1, state.game_name);
    copy_string(65, state.platform_name);
    copy_string(129, state.core_name);
    constexpr char app_name[] = "Azahar";
    std::memcpy(payload.data() + 193, app_name, sizeof(app_name) - 1);
    const u32 capabilities =
        cap_state | cap_remote_commands | cap_launch_by_id | GetRemoteFrontendExtraCapabilities();
    std::memcpy(payload.data() + 257, &capabilities, sizeof(capabilities));
    return payload;
}

} // namespace

struct SocketCallback {
    std::function<void(Response::Version)> version;
    std::function<void(Response::PortInfo)> port_info;
    std::function<void(Response::PadData)> pad_data;
};

class Socket {
public:
    using clock = std::chrono::system_clock;

    explicit Socket(const std::string& host, u16 port, u8 pad_index, u32 client_id,
                    SocketCallback callback)
        : callback(std::move(callback)), timer(io_context),
          socket(io_context, udp::endpoint(udp::v4(), 0)), client_id(client_id),
          pad_index(pad_index) {
        boost::system::error_code ec{};
        auto ipv4 = boost::asio::ip::make_address_v4(host, ec);
        if (ec.value() != boost::system::errc::success) {
            LOG_ERROR(Input, "Invalid IPv4 address \"{}\" provided to socket", host);
            ipv4 = boost::asio::ip::address_v4{};
        }

        send_endpoint = {udp::endpoint(ipv4, port)};
    }

    void Stop() {
        io_context.stop();
    }

    void Loop() {
        io_context.run();
    }

    void StartSend(const clock::time_point& from) {
        timer.expires_at(from + std::chrono::seconds(1));
        timer.async_wait([this](const boost::system::error_code& error) { HandleSend(error); });
    }

    void StartReceive() {
        socket.async_receive_from(
            boost::asio::buffer(receive_buffer), receive_endpoint,
            [this](const boost::system::error_code& error, std::size_t bytes_transferred) {
                HandleReceive(error, bytes_transferred);
            });
    }

private:
    void HandleReceive(const boost::system::error_code& error, std::size_t bytes_transferred) {
        if (error) {
            if (error == boost::asio::error::operation_aborted) {
                return;
            }
            LOG_DEBUG(Input, "DSU receive failed: {}", error.message());
            StartReceive();
            return;
        }
        // Reject control packets from any host or port other than the configured server.
        if (receive_endpoint != send_endpoint) {
            LOG_WARNING(Input, "Ignoring DSU packet from unexpected endpoint {}:{}",
                        receive_endpoint.address().to_string(), receive_endpoint.port());
            StartReceive();
            return;
        }
        if (const auto packet = ValidatePacket(receive_buffer.data(), bytes_transferred)) {
            switch (packet->type) {
            case static_cast<u32>(Type::Version): {
                if (packet->payload_size != sizeof(Response::Version)) {
                    break;
                }
                Response::Version version;
                std::memcpy(&version, packet->payload, sizeof(Response::Version));
                callback.version(std::move(version));
                break;
            }
            case static_cast<u32>(Type::PortInfo): {
                if (packet->payload_size != sizeof(Response::PortInfo)) {
                    break;
                }
                Response::PortInfo port_info;
                std::memcpy(&port_info, packet->payload, sizeof(Response::PortInfo));
                callback.port_info(std::move(port_info));
                break;
            }
            case static_cast<u32>(Type::PadData): {
                if (packet->payload_size != sizeof(Response::PadData)) {
                    break;
                }
                Response::PadData pad_data;
                std::memcpy(&pad_data, packet->payload, sizeof(Response::PadData));
                callback.pad_data(std::move(pad_data));
                break;
            }
            default:
                for (auto& response :
                     HandleExtendedRequest(packet->type, packet->payload, packet->payload_size)) {
                    SendExtended(response.type, response.payload);
                }
                break;
            }
        }
        StartReceive();
    }

    void SendExtended(u32 type, const std::vector<u8>& payload) {
        boost::system::error_code ignored{};
        const auto packet = BuildClientPacket(type, payload, client_id);
        socket.send_to(boost::asio::buffer(packet), send_endpoint, {}, ignored);
        if (ignored) {
            LOG_WARNING(Input, "Failed to send extended DSU packet 0x{:08X}: {}", type,
                        ignored.message());
        }
    }

    void HandleSend(const boost::system::error_code& error) {
        boost::system::error_code _ignored{};
        // Send a request for getting port info for the pad
        Request::PortInfo port_info{1, {pad_index, 0, 0, 0}};
        const auto port_message = Request::Create(port_info, client_id);
        std::memcpy(&send_buffer1, &port_message, PORT_INFO_SIZE);
        socket.send_to(boost::asio::buffer(send_buffer1), send_endpoint, {}, _ignored);

        // Send a request for getting pad data for the pad
        Request::PadData pad_data{Request::PadData::Flags::Id, pad_index, EMPTY_MAC_ADDRESS};
        const auto pad_message = Request::Create(pad_data, client_id);
        std::memcpy(send_buffer2.data(), &pad_message, PAD_DATA_SIZE);
        socket.send_to(boost::asio::buffer(send_buffer2), send_endpoint, {}, _ignored);

        SendExtended(static_cast<u32>(Type::State), BuildAzaharState());
        for (auto& response : TakePendingExtendedResponses()) {
            SendExtended(response.type, response.payload);
        }
        StartSend(timer.expiry());
    }

    SocketCallback callback;
    boost::asio::io_context io_context;
    boost::asio::basic_waitable_timer<clock> timer;
    udp::socket socket;

    u32 client_id{};
    u8 pad_index{};

    static constexpr std::size_t PORT_INFO_SIZE = sizeof(Message<Request::PortInfo>);
    static constexpr std::size_t PAD_DATA_SIZE = sizeof(Message<Request::PadData>);
    std::array<u8, PORT_INFO_SIZE> send_buffer1;
    std::array<u8, PAD_DATA_SIZE> send_buffer2;
    udp::endpoint send_endpoint;

    std::array<u8, EXTENDED_MAX_PACKET_SIZE> receive_buffer;
    udp::endpoint receive_endpoint;
};

static void SocketLoop(Socket* socket) {
    socket->StartReceive();
    socket->StartSend(Socket::clock::now());
    socket->Loop();
}

Client::Client(std::shared_ptr<DeviceStatus> status, const std::string& host, u16 port,
               u8 pad_index, u32 client_id)
    : status(std::move(status)) {
    StartCommunication(host, port, pad_index, client_id);
}

Client::~Client() {
    socket->Stop();
    thread.join();
}

void Client::ReloadSocket(const std::string& host, u16 port, u8 pad_index, u32 client_id) {
    socket->Stop();
    thread.join();
    StartCommunication(host, port, pad_index, client_id);
}

void Client::OnVersion(Response::Version data) {
    LOG_TRACE(Input, "Version packet received: {}", data.version);
}

void Client::OnPortInfo(Response::PortInfo data) {
    LOG_TRACE(Input, "PortInfo packet received: {}", data.model);
}

void Client::OnPadData(Response::PadData data) {
    LOG_TRACE(Input, "PadData packet received");
    const u32 incoming_sequence = data.packet_counter;
    // Treat the counter as a wrapping 32-bit sequence number.
    const u32 sequence_delta =
        packet_sequence ? incoming_sequence - *packet_sequence : std::numeric_limits<u32>::max();
    if (packet_sequence &&
        (sequence_delta == 0 || sequence_delta > std::numeric_limits<s32>::max())) {
        LOG_WARNING(
            Input,
            "PadData packet dropped because its stale info. Current count: {} Packet count: {}",
            *packet_sequence, incoming_sequence);
        return;
    }
    packet_sequence = incoming_sequence;
    // Due to differences between the 3ds and cemuhookudp motion directions, we need to invert
    // accel.x and accel.z and also invert pitch and yaw. See
    // https://github.com/citra-emu/citra/pull/4049 for more details on gyro/accel
    Common::Vec3f accel = Common::MakeVec<float>(-data.accel.x, data.accel.y, -data.accel.z);
    Common::Vec3f gyro = Common::MakeVec<float>(-data.gyro.pitch, -data.gyro.yaw, data.gyro.roll);
    {
        std::lock_guard guard(status->update_mutex);
        status->last_update = std::chrono::steady_clock::now();

        auto& pad = status->pad_status;
        pad.active = data.info.is_pad_active != 0;
        if (pad.active) {
            pad.digital_buttons = data.digital_button;
            pad.home = data.home != 0;
            pad.touch_hard_press = data.touch_hard_press != 0;
            pad.stick_axes = {data.left_stick_x, data.left_stick_y, data.right_stick_x,
                              data.right_stick_y};
            pad.analog_dpad = data.analog_button.dpad;
            pad.analog_face = data.analog_button.face;
            pad.analog_shoulder = data.analog_button.shoulder;
        } else {
            pad = {};
        }

        status->motion_status = pad.active ? std::make_tuple(accel, gyro)
                                           : std::make_tuple(Common::Vec3f{}, Common::Vec3f{});

        // TODO: add a setting for "click" touch. Click touch refers to a device that differentiates
        // between a simple "tap" and a hard press that causes the touch screen to click.
        const bool is_active = pad.active && data.touch_1.is_active != 0;

        float x = 0;
        float y = 0;

        if (is_active && status->touch_calibration) {
            const u16 min_x = status->touch_calibration->min_x;
            const u16 max_x = status->touch_calibration->max_x;
            const u16 min_y = status->touch_calibration->min_y;
            const u16 max_y = status->touch_calibration->max_y;

            if (max_x > min_x && max_y > min_y) {
                x = (std::clamp(static_cast<u16>(data.touch_1.x), min_x, max_x) - min_x) /
                    static_cast<float>(max_x - min_x);
                y = (std::clamp(static_cast<u16>(data.touch_1.y), min_y, max_y) - min_y) /
                    static_cast<float>(max_y - min_y);
            }
        }

        status->touch_status = {x, y, is_active};
    }
}

void Client::StartCommunication(const std::string& host, u16 port, u8 pad_index, u32 client_id) {
    packet_sequence.reset();
    {
        std::lock_guard guard(status->update_mutex);
        status->pad_status = {};
        status->motion_status = {};
        status->touch_status = {};
        status->last_update = {};
    }
    SocketCallback callback{[this](Response::Version version) { OnVersion(version); },
                            [this](Response::PortInfo info) { OnPortInfo(info); },
                            [this](Response::PadData data) { OnPadData(data); }};
    LOG_INFO(Input, "Starting communication with UDP input server on {}:{}", host, port);
    socket = std::make_unique<Socket>(host, port, pad_index, client_id, callback);
    thread = std::thread{SocketLoop, this->socket.get()};
}

void TestCommunication(const std::string& host, u16 port, u8 pad_index, u32 client_id,
                       const std::function<void()>& success_callback,
                       const std::function<void()>& failure_callback) {
    std::thread([=] {
        Common::Event success_event;
        SocketCallback callback{[](Response::Version version) {}, [](Response::PortInfo info) {},
                                [&](Response::PadData data) { success_event.Set(); }};
        Socket socket{host, port, pad_index, client_id, std::move(callback)};
        std::thread worker_thread{SocketLoop, &socket};
        bool result = success_event.WaitFor(std::chrono::seconds(8));
        socket.Stop();
        worker_thread.join();
        if (result) {
            success_callback();
        } else {
            failure_callback();
        }
    }).detach();
}

CalibrationConfigurationJob::CalibrationConfigurationJob(
    const std::string& host, u16 port, u8 pad_index, u32 client_id,
    std::function<void(Status)> status_callback,
    std::function<void(u16, u16, u16, u16)> data_callback) {

    std::thread([=, this] {
        u16 min_x{UINT16_MAX};
        u16 min_y{UINT16_MAX};
        u16 max_x{};
        u16 max_y{};

        Status current_status{Status::Initialized};
        SocketCallback callback{[](Response::Version version) {}, [](Response::PortInfo info) {},
                                [&](Response::PadData data) {
                                    constexpr u16 CALIBRATION_THRESHOLD = 100;

                                    if (current_status == Status::Initialized) {
                                        // Receiving data means the communication is ready now
                                        current_status = Status::Ready;
                                        status_callback(current_status);
                                    }
                                    if (!data.touch_1.is_active) {
                                        return;
                                    }
                                    LOG_DEBUG(Input, "Current touch: {} {}", data.touch_1.x,
                                              data.touch_1.y);
                                    min_x = std::min(min_x, static_cast<u16>(data.touch_1.x));
                                    min_y = std::min(min_y, static_cast<u16>(data.touch_1.y));
                                    if (current_status == Status::Ready) {
                                        // First touch - min data (min_x/min_y)
                                        current_status = Status::Stage1Completed;
                                        status_callback(current_status);
                                    }
                                    if (data.touch_1.x - min_x > CALIBRATION_THRESHOLD &&
                                        data.touch_1.y - min_y > CALIBRATION_THRESHOLD) {
                                        // Set the current position as max value and finishes
                                        // configuration
                                        max_x = data.touch_1.x;
                                        max_y = data.touch_1.y;
                                        current_status = Status::Completed;
                                        data_callback(min_x, min_y, max_x, max_y);
                                        status_callback(current_status);

                                        complete_event.Set();
                                    }
                                }};
        Socket socket{host, port, pad_index, client_id, std::move(callback)};
        std::thread worker_thread{SocketLoop, &socket};
        complete_event.Wait();
        socket.Stop();
        worker_thread.join();
    }).detach();
}

CalibrationConfigurationJob::~CalibrationConfigurationJob() {
    Stop();
}

void CalibrationConfigurationJob::Stop() {
    complete_event.Set();
}

} // namespace InputCommon::CemuhookUDP
