// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "audio_core/input.h"

namespace AudioCore {

/// Microphone capture through SDL2.
class SDL2Input final : public Input {
public:
    explicit SDL2Input(std::string device_id);
    ~SDL2Input() override;

    void StartSampling(const InputParameters& params) override;
    void StopSampling() override;
    bool IsSampling() override;
    void AdjustSampleRate(u32 sample_rate) override;
    Samples Read() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::string device_id;
};

std::vector<std::string> ListSDL2InputDevices();

} // namespace AudioCore
