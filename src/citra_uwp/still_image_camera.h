// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/frontend/camera/factory.h"
#include "core/frontend/camera/interface.h"

namespace UWP {

/// A PNG decoded once and shared by every camera instance that uses the same file.
struct StillImage {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba; ///< width * height * 4
};

/// Serves a fixed PNG as the camera feed; the configuration string is the file path.
class StillImageCameraFactory final : public Camera::CameraFactory {
public:
    std::unique_ptr<Camera::CameraInterface> Create(const std::string& config,
                                                    const Service::CAM::Flip& flip) override;

private:
    std::shared_ptr<const StillImage> LoadImage(const std::string& path);

    std::map<std::string, std::shared_ptr<const StillImage>> cache;
};

} // namespace UWP
