// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstddef>

#include <lodepng.h>

#include "citra_uwp/still_image_camera.h"
#include "common/logging/log.h"
#include "core/frontend/camera/blank_camera.h"
#include "core/hle/service/cam/cam.h"

namespace UWP {

namespace {

class StillImageCamera final : public Camera::CameraInterface {
public:
    StillImageCamera(std::shared_ptr<const StillImage> image_, const Service::CAM::Flip& flip)
        : image(std::move(image_)) {
        using Service::CAM::Flip;
        basic_flip_horizontal = flip == Flip::Horizontal || flip == Flip::Reverse;
        basic_flip_vertical = flip == Flip::Vertical || flip == Flip::Reverse;
        flip_horizontal = basic_flip_horizontal;
        flip_vertical = basic_flip_vertical;
    }

    void StartCapture() override {}
    void StopCapture() override {}

    void SetResolution(const Service::CAM::Resolution& resolution) override {
        width = resolution.width;
        height = resolution.height;
    }

    void SetFlip(Service::CAM::Flip flip) override {
        using Service::CAM::Flip;
        // Combine the title-requested flip with the configured orientation, like the desktop
        // backend.
        flip_horizontal =
            basic_flip_horizontal ^ (flip == Flip::Horizontal || flip == Flip::Reverse);
        flip_vertical = basic_flip_vertical ^ (flip == Flip::Vertical || flip == Flip::Reverse);
    }

    void SetEffect(Service::CAM::Effect effect) override {
        if (effect != Service::CAM::Effect::NoEffect) {
            LOG_WARNING(Service_CAM, "Camera effects are not implemented (requested {})",
                        static_cast<int>(effect));
        }
    }

    void SetFormat(Service::CAM::OutputFormat format) override {
        output_rgb = format == Service::CAM::OutputFormat::RGB565;
    }

    void SetFrameRate(Service::CAM::FrameRate frame_rate) override {}

    std::vector<u16> ReceiveFrame() override;

    bool IsPreviewAvailable() override {
        return image != nullptr;
    }

private:
    std::shared_ptr<const StillImage> image;
    int width = 640;
    int height = 480;
    bool output_rgb = false;
    bool flip_horizontal = false;
    bool flip_vertical = false;
    bool basic_flip_horizontal = false;
    bool basic_flip_vertical = false;
};

std::vector<u16> StillImageCamera::ReceiveFrame() {
    std::vector<u16> frame(static_cast<std::size_t>(width) * height, 0);
    if (!image || image->width == 0 || image->height == 0 || width <= 0 || height <= 0) {
        return frame;
    }

    // Aspect-fill with a centred crop, like the Qt camera's KeepAspectRatioByExpanding scale.
    const double scale = std::max(static_cast<double>(width) / image->width,
                                  static_cast<double>(height) / image->height);
    const double src_left = (image->width - width / scale) / 2.0;
    const double src_top = (image->height - height / scale) / 2.0;

    int previous_y = 0, previous_u = 0, previous_v = 0;
    auto dest = frame.begin();
    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < width; ++i) {
            const int sample_x = flip_horizontal ? width - 1 - i : i;
            const int sample_y = flip_vertical ? height - 1 - j : j;
            const u32 sx = static_cast<u32>(
                std::clamp<double>(src_left + sample_x / scale, 0.0, image->width - 1.0));
            const u32 sy = static_cast<u32>(
                std::clamp<double>(src_top + sample_y / scale, 0.0, image->height - 1.0));
            const u8* pixel = &image->rgba[(static_cast<std::size_t>(sy) * image->width + sx) * 4];
            const int r = pixel[0];
            const int g = pixel[1];
            const int b = pixel[2];

            if (output_rgb) {
                *(dest++) = static_cast<u16>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                continue;
            }

            // ITU-R BT.601 studio swing, the inverse of what Y2R applies on the way back.
            const int y = 16 + (66 * r + 129 * g + 25 * b) / 256;
            const int u = 128 + (-38 * r - 74 * g + 112 * b) / 256;
            const int v = 128 + (112 * r - 94 * g - 18 * b) / 256;
            if (i % 2 != 0) {
                const int pair_u = (previous_u + u) / 2;
                const int pair_v = (previous_v + v) / 2;
                *(dest++) = static_cast<u16>(std::clamp(previous_y, 0, 0xFF) |
                                             (std::clamp(pair_u, 0, 0xFF) << 8));
                *(dest++) =
                    static_cast<u16>(std::clamp(y, 0, 0xFF) | (std::clamp(pair_v, 0, 0xFF) << 8));
            } else {
                previous_y = y;
                previous_u = u;
                previous_v = v;
            }
        }
    }
    return frame;
}

} // namespace

std::shared_ptr<const StillImage> StillImageCameraFactory::LoadImage(const std::string& path) {
    const auto it = cache.find(path);
    if (it != cache.end()) {
        return it->second;
    }

    auto image = std::make_shared<StillImage>();
    const unsigned error = lodepng::decode(image->rgba, image->width, image->height, path);
    if (error != 0) {
        LOG_ERROR(Service_CAM, "Failed to decode camera image {}: {}", path,
                  lodepng_error_text(error));
        return nullptr;
    }
    cache.emplace(path, image);
    return image;
}

std::unique_ptr<Camera::CameraInterface> StillImageCameraFactory::Create(
    const std::string& config, const Service::CAM::Flip& flip) {
    if (config.empty()) {
        LOG_WARNING(Service_CAM, "Still-image camera selected without an image path; "
                                 "falling back to a blank feed");
        return std::make_unique<Camera::BlankCamera>();
    }
    auto image = LoadImage(config);
    if (!image) {
        return std::make_unique<Camera::BlankCamera>();
    }
    return std::make_unique<StillImageCamera>(std::move(image), flip);
}

} // namespace UWP
