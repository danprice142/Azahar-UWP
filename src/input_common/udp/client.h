// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include "common/common_types.h"
#include "common/thread.h"
#include "common/vector_math.h"

namespace InputCommon::CemuhookUDP {

constexpr u16 DEFAULT_PORT = 26760;
constexpr char DEFAULT_ADDR[] = "127.0.0.1";

class Socket;

namespace Response {
struct PadData;
struct PortInfo;
struct Version;
} // namespace Response

struct DeviceStatus {
    static constexpr std::chrono::milliseconds PACKET_TIMEOUT{1000};

    struct PadStatus {
        bool active{};
        u16 digital_buttons{};
        bool home{};
        bool touch_hard_press{};
        // Left X/Y followed by right X/Y. DSU uses 128 as neutral, with positive Y upward.
        std::array<u8, 4> stick_axes{128, 128, 128, 128};
        std::array<u8, 4> analog_dpad{};
        std::array<u8, 4> analog_face{};
        std::array<u8, 4> analog_shoulder{};
    };

    std::mutex update_mutex;
    std::tuple<Common::Vec3<float>, Common::Vec3<float>> motion_status;
    std::tuple<float, float, bool> touch_status;
    PadStatus pad_status;
    std::chrono::steady_clock::time_point last_update{};

    // calibration data for scaling the device's touch area to 3ds
    struct CalibrationData {
        u16 min_x{};
        u16 min_y{};
        u16 max_x{};
        u16 max_y{};
    };
    std::optional<CalibrationData> touch_calibration;
};

class Client {
public:
    explicit Client(std::shared_ptr<DeviceStatus> status, const std::string& host = DEFAULT_ADDR,
                    u16 port = DEFAULT_PORT, u8 pad_index = 0, u32 client_id = 24872);
    ~Client();
    void ReloadSocket(const std::string& host = "127.0.0.1", u16 port = 26760, u8 pad_index = 0,
                      u32 client_id = 24872);

private:
    void OnVersion(Response::Version);
    void OnPortInfo(Response::PortInfo);
    void OnPadData(Response::PadData);
    void StartCommunication(const std::string& host, u16 port, u8 pad_index, u32 client_id);

    std::unique_ptr<Socket> socket;
    std::shared_ptr<DeviceStatus> status;
    std::thread thread;
    std::optional<u32> packet_sequence;
};

/// An async job allowing configuration of the touchpad calibration.
class CalibrationConfigurationJob {
public:
    enum class Status {
        Initialized,
        Ready,
        Stage1Completed,
        Completed,
    };
    /**
     * Constructs and starts the job with the specified parameter.
     *
     * @param status_callback Callback for job status updates
     * @param data_callback Called when calibration data is ready
     */
    explicit CalibrationConfigurationJob(const std::string& host, u16 port, u8 pad_index,
                                         u32 client_id, std::function<void(Status)> status_callback,
                                         std::function<void(u16, u16, u16, u16)> data_callback);
    ~CalibrationConfigurationJob();
    void Stop();

private:
    Common::Event complete_event;
};

void TestCommunication(const std::string& host, u16 port, u8 pad_index, u32 client_id,
                       const std::function<void()>& success_callback,
                       const std::function<void()>& failure_callback);

} // namespace InputCommon::CemuhookUDP
