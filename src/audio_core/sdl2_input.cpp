// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "audio_core/sdl2_input.h"

#include <cstring>
#include <utility>

#include <SDL.h>

#include "audio_core/sink.h"
#include "common/logging/log.h"
#include "common/threadsafe_queue.h"

namespace AudioCore {

using SampleQueue = Common::SPSCQueue<Samples>;

struct SDL2Input::Impl {
    SDL_AudioDeviceID device{};
    SampleQueue sample_queue{};
    u8 output_sample_size{};
    Signedness output_signedness{Signedness::Signed};

    static void CaptureCallback(void* user_data, Uint8* stream, int length);
};

SDL2Input::SDL2Input(std::string device_id_)
    : impl(std::make_unique<Impl>()), device_id(std::move(device_id_)) {}

SDL2Input::~SDL2Input() {
    StopSampling();
}

void SDL2Input::StartSampling(const InputParameters& params) {
    if (IsSampling()) {
        return;
    }
    if (params.sample_size != 8 && params.sample_size != 16) {
        LOG_ERROR(Audio, "SDL microphone cannot supply {}-bit PCM", params.sample_size);
        return;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        LOG_ERROR(Audio, "SDL microphone audio initialization failed: {}", SDL_GetError());
        return;
    }

    parameters = params;
    impl->output_sample_size = params.sample_size;
    impl->output_signedness = params.sign;

    SDL_AudioSpec desired{};
    desired.freq = static_cast<int>(params.sample_rate);
    desired.format = AUDIO_S16SYS;
    desired.channels = 1;
    desired.samples = 512;
    desired.callback = &Impl::CaptureCallback;
    desired.userdata = impl.get();

    SDL_AudioSpec obtained{};
    const char* requested_device =
        device_id.empty() || device_id == auto_device_name ? nullptr : device_id.c_str();
    impl->device =
        SDL_OpenAudioDevice(requested_device, SDL_TRUE, &desired, &obtained, /*allowed_changes=*/0);
    if (impl->device == 0) {
        LOG_ERROR(Audio, "SDL microphone open failed: {}", SDL_GetError());
        return;
    }
    if (obtained.format != AUDIO_S16SYS || obtained.channels != 1 ||
        obtained.freq != desired.freq) {
        LOG_ERROR(Audio,
                  "SDL microphone returned an unexpected format (format {}, channels {}, rate {})",
                  obtained.format, obtained.channels, obtained.freq);
        StopSampling();
        return;
    }

    SDL_PauseAudioDevice(impl->device, SDL_FALSE);
    LOG_INFO(Audio, "SDL microphone capture started at {} Hz", params.sample_rate);
}

void SDL2Input::StopSampling() {
    if (impl->device != 0) {
        SDL_PauseAudioDevice(impl->device, SDL_TRUE);
        SDL_CloseAudioDevice(impl->device);
        impl->device = 0;
    }

    Samples discarded;
    while (impl->sample_queue.Pop(discarded)) {
    }
}

bool SDL2Input::IsSampling() {
    return impl->device != 0;
}

void SDL2Input::AdjustSampleRate(u32 sample_rate) {
    if (!IsSampling() || sample_rate == parameters.sample_rate) {
        parameters.sample_rate = sample_rate;
        return;
    }

    auto new_parameters = parameters;
    new_parameters.sample_rate = sample_rate;
    StopSampling();
    StartSampling(new_parameters);
}

Samples SDL2Input::Read() {
    if (!IsSampling()) {
        return {};
    }

    Samples result;
    Samples chunk;
    while (impl->sample_queue.Pop(chunk)) {
        result.insert(result.end(), chunk.begin(), chunk.end());
    }
    return result;
}

void SDL2Input::Impl::CaptureCallback(void* user_data, Uint8* stream, int length) {
    auto* self = static_cast<Impl*>(user_data);
    if (self == nullptr || stream == nullptr || length <= 0) {
        return;
    }

    const std::size_t frame_count = static_cast<std::size_t>(length) / sizeof(s16);
    Samples samples;
    samples.reserve(frame_count * (self->output_sample_size / 8));

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        s16 sample{};
        std::memcpy(&sample, stream + frame * sizeof(sample), sizeof(sample));
        if (self->output_sample_size == 8) {
            s16 scaled = static_cast<s16>(sample >> 8);
            if (self->output_signedness == Signedness::Unsigned) {
                scaled += 128;
            }
            samples.push_back(static_cast<u8>(scaled));
        } else {
            u16 encoded = static_cast<u16>(sample);
            if (self->output_signedness == Signedness::Unsigned) {
                encoded ^= 0x8000;
            }
            samples.push_back(static_cast<u8>(encoded & 0xFF));
            samples.push_back(static_cast<u8>(encoded >> 8));
        }
    }
    if (!samples.empty()) {
        self->sample_queue.Push(std::move(samples));
    }
}

std::vector<std::string> ListSDL2InputDevices() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        LOG_WARNING(Audio, "Could not initialize SDL while listing microphones: {}",
                    SDL_GetError());
        return {};
    }

    const int count = SDL_GetNumAudioDevices(SDL_TRUE);
    if (count < 0) {
        LOG_WARNING(Audio, "Could not enumerate SDL microphones: {}", SDL_GetError());
        return {};
    }

    std::vector<std::string> devices;
    devices.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        if (const char* name = SDL_GetAudioDeviceName(index, SDL_TRUE); name != nullptr) {
            devices.emplace_back(name);
        }
    }
    return devices;
}

} // namespace AudioCore
