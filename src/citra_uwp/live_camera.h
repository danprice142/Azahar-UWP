// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/frontend/camera/factory.h"

namespace UWP {

struct LiveCameraDevice {
    /// MediaFrameSourceGroup identifier persisted in camera_config.
    std::string id;
    std::string name;
};

/// Enumerates colour-capable MediaFrameSource groups. Must not be called on the UI thread.
std::vector<LiveCameraDevice> EnumerateLiveCameraDevices();

/// Captures the CoreWindow dispatcher for the camera consent prompt. Call once on the UI thread.
bool InitializeLiveCameraUiDispatcher();

class LiveCameraHandler;

/**
 * Live-camera backend using MediaCapture/MediaFrameReader. Cameras that select the same source
 * share one handler, since a webcam normally permits only one MediaCapture session.
 */
class LiveCameraFactory final : public Camera::CameraFactory {
public:
    std::unique_ptr<Camera::CameraInterface> Create(const std::string& config,
                                                    const Service::CAM::Flip& flip) override;

private:
    std::mutex handlers_mutex;
    std::unordered_map<std::string, std::weak_ptr<LiveCameraHandler>> handlers;
};

} // namespace UWP
