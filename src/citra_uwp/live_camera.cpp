// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "citra_uwp/live_camera.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

#include <unknwn.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Capture.Frames.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.UI.Core.h>

#include "common/logging/log.h"
#include "core/frontend/camera/interface.h"
#include "core/hle/service/cam/cam.h"

namespace {

// The zero-copy byte access interface is not part of the projected WinRT surface.
struct __declspec(uuid("5B0D3235-4DBA-4D44-8659-1D2899D96990")) IMemoryBufferByteAccess
    : ::IUnknown {
    virtual HRESULT __stdcall GetBuffer(std::uint8_t** value, std::uint32_t* capacity) = 0;
};

class ApartmentScope {
public:
    ApartmentScope() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }

    ~ApartmentScope() {
        winrt::uninit_apartment();
    }
};

using winrt::Windows::Media::Capture::Frames::MediaFrameSourceGroup;
using winrt::Windows::Media::Capture::Frames::MediaFrameSourceInfo;
using winrt::Windows::Media::Capture::Frames::MediaFrameSourceKind;

struct SourceGroup {
    UWP::LiveCameraDevice device;
    MediaFrameSourceGroup group{nullptr};
};

bool HasColourSource(const MediaFrameSourceGroup& group) {
    for (const MediaFrameSourceInfo& info : group.SourceInfos()) {
        if (info.SourceKind() == MediaFrameSourceKind::Color) {
            return true;
        }
    }
    return false;
}

std::vector<SourceGroup> FindSourceGroups() {
    std::vector<SourceGroup> result;
    const auto groups = MediaFrameSourceGroup::FindAllAsync().get();
    result.reserve(groups.Size());
    for (const MediaFrameSourceGroup& group : groups) {
        if (!HasColourSource(group)) {
            continue;
        }
        SourceGroup entry;
        entry.device.id = winrt::to_string(group.Id());
        entry.device.name = winrt::to_string(group.DisplayName());
        if (entry.device.name.empty()) {
            entry.device.name = entry.device.id;
        }
        entry.group = group;
        result.emplace_back(std::move(entry));
    }
    return result;
}

enum class CameraConsentState {
    NotStarted,
    Pending,
    Granted,
    Failed,
};

std::mutex camera_consent_mutex;
std::condition_variable camera_consent_changed;
winrt::Windows::UI::Core::CoreDispatcher camera_ui_dispatcher{nullptr};
CameraConsentState camera_consent_state = CameraConsentState::NotStarted;
std::string camera_consent_error;

void CompleteCameraConsent(bool granted, std::string error = {}) {
    {
        std::scoped_lock lock{camera_consent_mutex};
        camera_consent_state = granted ? CameraConsentState::Granted : CameraConsentState::Failed;
        camera_consent_error = std::move(error);
    }
    camera_consent_changed.notify_all();
}

/// Performs the process's first MediaCapture initialization on the UI apartment, which Windows
/// requires for the privacy-consent prompt.
winrt::fire_and_forget InitializeCameraConsentOnUi(std::string source_group_id) {
    try {
        const auto groups = co_await MediaFrameSourceGroup::FindAllAsync();
        const auto selected =
            std::find_if(groups.begin(), groups.end(), [&source_group_id](const auto& group) {
                return HasColourSource(group) &&
                       (source_group_id.empty() || winrt::to_string(group.Id()) == source_group_id);
            });
        if (selected == groups.end()) {
            throw std::runtime_error{"The requested live camera was not found."};
        }

        using winrt::Windows::Media::Capture::MediaCapture;
        using winrt::Windows::Media::Capture::MediaCaptureInitializationSettings;
        using winrt::Windows::Media::Capture::MediaCaptureMemoryPreference;
        using winrt::Windows::Media::Capture::StreamingCaptureMode;

        MediaCaptureInitializationSettings settings;
        settings.SourceGroup(*selected);
        settings.StreamingCaptureMode(StreamingCaptureMode::Video);
        settings.MemoryPreference(MediaCaptureMemoryPreference::Cpu);

        MediaCapture consent_capture;
        co_await consent_capture.InitializeAsync(settings);
        consent_capture.as<winrt::Windows::Foundation::IClosable>().Close();
        CompleteCameraConsent(true);
    } catch (const winrt::hresult_error& error) {
        CompleteCameraConsent(false, "Camera access was not granted: " +
                                         winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        CompleteCameraConsent(false, error.what());
    }
}

bool EnsureCameraConsent(const std::string& source_group_id,
                         const std::atomic_bool& stop_requested) {
    winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
    bool request_consent = false;
    {
        std::scoped_lock lock{camera_consent_mutex};
        if (camera_consent_state == CameraConsentState::Granted) {
            return true;
        }
        if (camera_consent_state != CameraConsentState::Pending) {
            if (!camera_ui_dispatcher) {
                camera_consent_state = CameraConsentState::Failed;
                camera_consent_error = "The CoreWindow camera dispatcher was not initialized.";
            } else {
                camera_consent_state = CameraConsentState::Pending;
                camera_consent_error.clear();
                dispatcher = camera_ui_dispatcher;
                request_consent = true;
            }
        }
    }

    if (request_consent) {
        try {
            const auto dispatched = dispatcher.RunAsync(
                winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                [source_group_id] { InitializeCameraConsentOnUi(source_group_id); });
            (void)dispatched;
        } catch (const winrt::hresult_error& error) {
            CompleteCameraConsent(false, "Could not request camera access on the UI thread: " +
                                             winrt::to_string(error.message()));
        }
    }

    std::unique_lock lock{camera_consent_mutex};
    while (camera_consent_state == CameraConsentState::Pending &&
           !stop_requested.load(std::memory_order_acquire)) {
        // A timed wait lets destruction proceed if the dispatcher never invokes the callback.
        camera_consent_changed.wait_for(lock, std::chrono::milliseconds{100});
    }
    if (camera_consent_state == CameraConsentState::Granted) {
        return true;
    }
    if (!stop_requested.load(std::memory_order_acquire)) {
        LOG_WARNING(Service_CAM, "Live camera permission initialization failed: {}",
                    camera_consent_error);
    }
    return false;
}

} // namespace

namespace UWP {

bool InitializeLiveCameraUiDispatcher() {
    try {
        const auto core_window = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread();
        if (!core_window || !core_window.Dispatcher()) {
            LOG_ERROR(Service_CAM, "Live camera cannot capture the current CoreWindow dispatcher");
            return false;
        }
        {
            std::scoped_lock lock{camera_consent_mutex};
            camera_ui_dispatcher = core_window.Dispatcher();
        }
        return true;
    } catch (const winrt::hresult_error& error) {
        LOG_ERROR(Service_CAM, "Live camera UI dispatcher initialization failed: {}",
                  winrt::to_string(error.message()));
        return false;
    }
}

std::vector<LiveCameraDevice> EnumerateLiveCameraDevices() {
    try {
        ApartmentScope apartment;
        const auto source_groups = FindSourceGroups();
        std::vector<LiveCameraDevice> devices;
        devices.reserve(source_groups.size());
        for (const auto& source_group : source_groups) {
            devices.push_back(source_group.device);
        }
        return devices;
    } catch (const winrt::hresult_error& error) {
        LOG_WARNING(Service_CAM, "Live camera enumeration failed: {}",
                    winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        LOG_WARNING(Service_CAM, "Live camera enumeration failed: {}", error.what());
    }
    return {};
}

class LiveCameraHandler {
public:
    struct Frame {
        u32 width = 0;
        u32 height = 0;
        std::vector<u8> bgra;
    };

    explicit LiveCameraHandler(std::string source_group_id_)
        : source_group_id{std::move(source_group_id_)} {
        // Start the thread only after every member below has been constructed.
        capture_thread = std::thread{&LiveCameraHandler::CaptureThread, this};
    }

    ~LiveCameraHandler() {
        stop_requested.store(true, std::memory_order_release);
        state_changed.notify_all();
        if (capture_thread.joinable()) {
            capture_thread.join();
        }
    }

    void StartCapture() {
        start_generation.fetch_add(1, std::memory_order_acq_rel);
        active_clients.fetch_add(1, std::memory_order_acq_rel);
        state_changed.notify_all();
    }

    void StopCapture() {
        unsigned clients = active_clients.load(std::memory_order_acquire);
        while (clients != 0 && !active_clients.compare_exchange_weak(clients, clients - 1,
                                                                     std::memory_order_acq_rel)) {
        }
        state_changed.notify_all();
    }

    bool IsAvailable() const {
        return available.load(std::memory_order_acquire);
    }

    std::shared_ptr<const Frame> GetLatestFrame() const {
        std::scoped_lock lock{frame_mutex};
        return latest_frame;
    }

private:
    void CaptureThread();
    void RunCaptureSession();
    void CopyLatestFrame(const winrt::Windows::Media::Capture::Frames::MediaFrameReader& reader);

    std::string source_group_id;
    std::thread capture_thread;
    std::atomic_bool stop_requested{false};
    std::atomic_bool available{false};
    std::atomic_uint active_clients{0};
    std::atomic_uint64_t start_generation{0};
    std::atomic_uint64_t failed_session_generation{0};
    std::uint64_t next_session_generation = 0;
    mutable std::mutex frame_mutex;
    std::mutex callback_mutex;
    std::mutex state_mutex;
    std::condition_variable state_changed;
    std::shared_ptr<const Frame> latest_frame;
};

void LiveCameraHandler::CaptureThread() {
    try {
        ApartmentScope apartment;
        while (!stop_requested.load(std::memory_order_acquire)) {
            {
                std::unique_lock lock{state_mutex};
                state_changed.wait(lock, [this] {
                    return stop_requested.load(std::memory_order_acquire) ||
                           active_clients.load(std::memory_order_acquire) != 0;
                });
            }
            if (stop_requested.load(std::memory_order_acquire)) {
                break;
            }
            RunCaptureSession();
        }
    } catch (const winrt::hresult_error& error) {
        LOG_ERROR(Service_CAM, "Live camera worker failed: {}", winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        LOG_ERROR(Service_CAM, "Live camera worker failed: {}", error.what());
    }
    available.store(false, std::memory_order_release);
}

void LiveCameraHandler::RunCaptureSession() {
    using winrt::Windows::Media::Capture::MediaCapture;
    using winrt::Windows::Media::Capture::MediaCaptureInitializationSettings;
    using winrt::Windows::Media::Capture::MediaCaptureMemoryPreference;
    using winrt::Windows::Media::Capture::MediaStreamType;
    using winrt::Windows::Media::Capture::StreamingCaptureMode;
    using winrt::Windows::Media::Capture::Frames::MediaFrameReaderStartStatus;
    using winrt::Windows::Media::MediaProperties::MediaEncodingSubtypes;

    MediaCapture capture;
    winrt::Windows::Media::Capture::Frames::MediaFrameReader reader{nullptr};
    winrt::event_token frame_arrived_token{};
    winrt::event_token capture_failed_token{};
    bool frame_handler_attached = false;
    bool failed_handler_attached = false;
    bool reader_started = false;
    bool attempt_failed = false;
    const std::uint64_t attempted_start_generation =
        start_generation.load(std::memory_order_acquire);
    const std::uint64_t session_generation = ++next_session_generation;

    try {
        if (!EnsureCameraConsent(source_group_id, stop_requested)) {
            if (stop_requested.load(std::memory_order_acquire)) {
                return;
            }
            throw winrt::hresult_error{winrt::hresult{static_cast<std::int32_t>(0x80070005u)}};
        }

        auto groups = FindSourceGroups();
        const auto selected = std::find_if(groups.begin(), groups.end(), [this](const auto& entry) {
            return source_group_id.empty() || entry.device.id == source_group_id;
        });
        if (selected == groups.end()) {
            LOG_WARNING(Service_CAM, "Requested live camera '{}' was not found",
                        source_group_id.empty() ? "<default>" : source_group_id);
            throw winrt::hresult_error{winrt::hresult{static_cast<std::int32_t>(0x8007048Fu)}};
        }

        MediaCaptureInitializationSettings settings;
        settings.SourceGroup(selected->group);
        settings.StreamingCaptureMode(StreamingCaptureMode::Video);
        settings.MemoryPreference(MediaCaptureMemoryPreference::Cpu);
        capture.InitializeAsync(settings).get();
        capture_failed_token =
            capture.Failed([this, session_generation](const auto&, const auto& failure) {
                std::scoped_lock callback_lock{callback_mutex};
                failed_session_generation.store(session_generation, std::memory_order_release);
                state_changed.notify_all();
                LOG_WARNING(Service_CAM, "Live camera session failed (0x{:08X}): {}",
                            failure.Code(), winrt::to_string(failure.Message()));
            });
        failed_handler_attached = true;

        winrt::Windows::Media::Capture::Frames::MediaFrameSource frame_source{nullptr};
        for (const auto& item : capture.FrameSources()) {
            const auto source = item.Value();
            if (source.Info().SourceKind() != MediaFrameSourceKind::Color) {
                continue;
            }
            if (!frame_source || source.Info().MediaStreamType() == MediaStreamType::VideoPreview) {
                frame_source = source;
            }
            if (source.Info().MediaStreamType() == MediaStreamType::VideoPreview) {
                break;
            }
        }
        if (!frame_source) {
            throw winrt::hresult_error{winrt::hresult{static_cast<std::int32_t>(0x8007048Fu)}};
        }

        // The 3DS camera service never requests more than VGA; pick the closest source format.
        winrt::Windows::Media::Capture::Frames::MediaFrameFormat preferred_format{nullptr};
        std::uint64_t preferred_score = std::numeric_limits<std::uint64_t>::max();
        for (const auto& format : frame_source.SupportedFormats()) {
            const auto video_format = format.VideoFormat();
            if (!video_format) {
                continue;
            }
            constexpr std::uint32_t target_width = 640;
            constexpr std::uint32_t target_height = 480;
            const std::uint32_t format_width = video_format.Width();
            const std::uint32_t format_height = video_format.Height();
            const std::uint64_t undersize_penalty =
                (format_width < target_width || format_height < target_height) ? 1'000'000 : 0;
            const std::uint64_t score =
                undersize_penalty +
                static_cast<std::uint64_t>(
                    std::abs(static_cast<std::int64_t>(format_width) - target_width)) +
                static_cast<std::uint64_t>(
                    std::abs(static_cast<std::int64_t>(format_height) - target_height));
            if (score < preferred_score) {
                preferred_score = score;
                preferred_format = format;
            }
        }
        if (preferred_format) {
            try {
                frame_source.SetFormatAsync(preferred_format).get();
            } catch (const winrt::hresult_error& error) {
                LOG_INFO(Service_CAM, "Could not select the preferred VGA camera format: {}",
                         winrt::to_string(error.message()));
            }
        }

        // BGRA8 in CPU memory keeps the callback independent of D3D devices.
        try {
            reader =
                capture.CreateFrameReaderAsync(frame_source, MediaEncodingSubtypes::Bgra8()).get();
        } catch (const winrt::hresult_error& error) {
            // Some webcams decline the conversion at reader creation; CPU frames are converted
            // below.
            LOG_INFO(Service_CAM, "BGRA camera reader unavailable ({}); using native format",
                     winrt::to_string(error.message()));
            reader = capture.CreateFrameReaderAsync(frame_source).get();
        }
        frame_arrived_token = reader.FrameArrived([this](const auto& sender, const auto&) {
            std::scoped_lock callback_lock{callback_mutex};
            CopyLatestFrame(sender);
        });
        frame_handler_attached = true;

        const auto start_status = reader.StartAsync().get();
        if (start_status != MediaFrameReaderStartStatus::Success) {
            LOG_WARNING(Service_CAM, "Starting live camera '{}' returned status {}",
                        selected->device.name, static_cast<int>(start_status));
            throw winrt::hresult_error{winrt::hresult{static_cast<std::int32_t>(0x80004005u)}};
        }
        reader_started = true;
        available.store(true, std::memory_order_release);
        LOG_INFO(Service_CAM, "Capturing live camera '{}'", selected->device.name);

        std::unique_lock lock{state_mutex};
        state_changed.wait(lock, [this, session_generation] {
            return stop_requested.load(std::memory_order_acquire) ||
                   active_clients.load(std::memory_order_acquire) == 0 ||
                   failed_session_generation.load(std::memory_order_acquire) == session_generation;
        });
        attempt_failed =
            failed_session_generation.load(std::memory_order_acquire) == session_generation;
    } catch (const winrt::hresult_error& error) {
        attempt_failed = true;
        LOG_WARNING(Service_CAM, "Opening live camera failed: {}",
                    winrt::to_string(error.message()));
    }

    available.store(false, std::memory_order_release);
    if (frame_handler_attached) {
        try {
            reader.FrameArrived(frame_arrived_token);
        } catch (const winrt::hresult_error&) {
        }
    }
    if (failed_handler_attached) {
        try {
            capture.Failed(capture_failed_token);
        } catch (const winrt::hresult_error&) {
        }
    }
    if (reader_started) {
        try {
            reader.StopAsync().get();
        } catch (const winrt::hresult_error& error) {
            LOG_WARNING(Service_CAM, "Stopping live camera failed: {}",
                        winrt::to_string(error.message()));
        }
    }
    // Taking the lock waits for a callback that was already running.
    {
        std::scoped_lock callback_lock{callback_mutex};
        std::scoped_lock frame_lock{frame_mutex};
        latest_frame.reset();
    }

    if (attempt_failed && !stop_requested.load(std::memory_order_acquire) &&
        active_clients.load(std::memory_order_acquire) != 0) {
        // Avoid retrying continuously while a title holds a failed camera open.
        std::unique_lock lock{state_mutex};
        state_changed.wait(lock, [this, attempted_start_generation] {
            return stop_requested.load(std::memory_order_acquire) ||
                   active_clients.load(std::memory_order_acquire) == 0 ||
                   start_generation.load(std::memory_order_acquire) != attempted_start_generation;
        });
    }
}

void LiveCameraHandler::CopyLatestFrame(
    const winrt::Windows::Media::Capture::Frames::MediaFrameReader& reader) {
    using winrt::Windows::Graphics::Imaging::BitmapAlphaMode;
    using winrt::Windows::Graphics::Imaging::BitmapBufferAccessMode;
    using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
    using winrt::Windows::Graphics::Imaging::SoftwareBitmap;

    try {
        const auto frame_reference = reader.TryAcquireLatestFrame();
        if (!frame_reference) {
            return;
        }
        const auto video_frame = frame_reference.VideoMediaFrame();
        if (!video_frame) {
            return;
        }
        auto bitmap = video_frame.SoftwareBitmap();
        if (!bitmap) {
            return;
        }
        if (bitmap.BitmapPixelFormat() != BitmapPixelFormat::Bgra8) {
            bitmap =
                SoftwareBitmap::Convert(bitmap, BitmapPixelFormat::Bgra8, BitmapAlphaMode::Ignore);
        }

        const u32 width = static_cast<u32>(bitmap.PixelWidth());
        const u32 height = static_cast<u32>(bitmap.PixelHeight());
        if (width == 0 || height == 0) {
            return;
        }

        const auto bitmap_buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Read);
        const auto plane = bitmap_buffer.GetPlaneDescription(0);
        const auto memory_reference = bitmap_buffer.CreateReference();
        const auto byte_access = memory_reference.as<IMemoryBufferByteAccess>();
        std::uint8_t* bytes = nullptr;
        std::uint32_t capacity = 0;
        winrt::check_hresult(byte_access->GetBuffer(&bytes, &capacity));

        constexpr std::size_t bytes_per_pixel = 4;
        const std::size_t row_bytes = static_cast<std::size_t>(width) * bytes_per_pixel;
        const std::size_t required_size = row_bytes * height;
        auto frame = std::make_shared<Frame>();
        frame->width = width;
        frame->height = height;
        frame->bgra.resize(required_size);

        const std::ptrdiff_t stride = plane.Stride;
        const std::size_t absolute_stride =
            static_cast<std::size_t>(stride >= 0 ? stride : -stride);
        const std::size_t last_row_offset =
            static_cast<std::size_t>(plane.StartIndex) + absolute_stride * (height - 1);
        if (bytes == nullptr || row_bytes > absolute_stride ||
            last_row_offset + row_bytes > capacity) {
            return;
        }

        for (u32 y = 0; y < height; ++y) {
            const std::size_t source_row =
                stride >= 0 ? static_cast<std::size_t>(plane.StartIndex) + absolute_stride * y
                            : last_row_offset - absolute_stride * y;
            std::memcpy(frame->bgra.data() + row_bytes * y, bytes + source_row, row_bytes);
        }

        std::scoped_lock lock{frame_mutex};
        latest_frame = std::move(frame);
    } catch (const winrt::hresult_error& error) {
        LOG_WARNING(Service_CAM, "Reading a live camera frame failed: {}",
                    winrt::to_string(error.message()));
    }
}

namespace {

class LiveCamera final : public Camera::CameraInterface {
public:
    LiveCamera(std::shared_ptr<LiveCameraHandler> handler_, const Service::CAM::Flip& flip)
        : handler{std::move(handler_)} {
        using Service::CAM::Flip;
        basic_flip_horizontal = flip == Flip::Horizontal || flip == Flip::Reverse;
        basic_flip_vertical = flip == Flip::Vertical || flip == Flip::Reverse;
        flip_horizontal = basic_flip_horizontal;
        flip_vertical = basic_flip_vertical;
    }

    ~LiveCamera() override {
        StopCapture();
    }

    void StartCapture() override {
        if (!capturing) {
            handler->StartCapture();
            capturing = true;
        }
    }

    void StopCapture() override {
        if (capturing) {
            handler->StopCapture();
            capturing = false;
        }
    }

    void SetResolution(const Service::CAM::Resolution& resolution) override {
        width = resolution.width;
        height = resolution.height;
    }

    void SetFlip(Service::CAM::Flip flip) override {
        using Service::CAM::Flip;
        flip_horizontal =
            basic_flip_horizontal ^ (flip == Flip::Horizontal || flip == Flip::Reverse);
        flip_vertical = basic_flip_vertical ^ (flip == Flip::Vertical || flip == Flip::Reverse);
    }

    void SetEffect(Service::CAM::Effect effect) override {
        if (effect != Service::CAM::Effect::NoEffect) {
            LOG_WARNING(Service_CAM, "Live-camera effects are not implemented (requested {})",
                        static_cast<int>(effect));
        }
    }

    void SetFormat(Service::CAM::OutputFormat format) override {
        output_rgb = format == Service::CAM::OutputFormat::RGB565;
    }

    void SetFrameRate(Service::CAM::FrameRate frame_rate) override {}

    std::vector<u16> ReceiveFrame() override {
        std::vector<u16> output(static_cast<std::size_t>(width) * height, 0);
        const auto source = handler->GetLatestFrame();
        if (!source || source->width == 0 || source->height == 0 || source->bgra.empty() ||
            width <= 0 || height <= 0) {
            return output;
        }

        // Aspect-fill with a centred crop, matching the desktop camera backend.
        const double scale = std::max(static_cast<double>(width) / source->width,
                                      static_cast<double>(height) / source->height);
        const double source_left = (source->width - width / scale) / 2.0;
        const double source_top = (source->height - height / scale) / 2.0;

        int previous_y = 0;
        int previous_u = 0;
        int previous_v = 0;
        auto destination = output.begin();
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int sample_x = flip_horizontal ? width - 1 - x : x;
                const int sample_y = flip_vertical ? height - 1 - y : y;
                const u32 source_x = static_cast<u32>(
                    std::clamp<double>(source_left + sample_x / scale, 0.0, source->width - 1.0));
                const u32 source_y = static_cast<u32>(
                    std::clamp<double>(source_top + sample_y / scale, 0.0, source->height - 1.0));
                const u8* pixel =
                    &source->bgra[(static_cast<std::size_t>(source_y) * source->width + source_x) *
                                  4];
                const int b = pixel[0];
                const int g = pixel[1];
                const int r = pixel[2];

                if (output_rgb) {
                    *(destination++) =
                        static_cast<u16>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                    continue;
                }

                const int luma = 16 + (66 * r + 129 * g + 25 * b) / 256;
                const int chroma_u = 128 + (-38 * r - 74 * g + 112 * b) / 256;
                const int chroma_v = 128 + (112 * r - 94 * g - 18 * b) / 256;
                if (x % 2 != 0) {
                    const int pair_u = (previous_u + chroma_u) / 2;
                    const int pair_v = (previous_v + chroma_v) / 2;
                    *(destination++) = static_cast<u16>(std::clamp(previous_y, 0, 0xFF) |
                                                        (std::clamp(pair_u, 0, 0xFF) << 8));
                    *(destination++) = static_cast<u16>(std::clamp(luma, 0, 0xFF) |
                                                        (std::clamp(pair_v, 0, 0xFF) << 8));
                } else {
                    previous_y = luma;
                    previous_u = chroma_u;
                    previous_v = chroma_v;
                }
            }
        }
        return output;
    }

    bool IsPreviewAvailable() override {
        return handler->IsAvailable();
    }

private:
    std::shared_ptr<LiveCameraHandler> handler;
    int width = 640;
    int height = 480;
    bool capturing = false;
    bool output_rgb = false;
    bool flip_horizontal = false;
    bool flip_vertical = false;
    bool basic_flip_horizontal = false;
    bool basic_flip_vertical = false;
};

} // namespace

std::unique_ptr<Camera::CameraInterface> LiveCameraFactory::Create(const std::string& config,
                                                                   const Service::CAM::Flip& flip) {
    std::shared_ptr<LiveCameraHandler> handler;
    {
        std::scoped_lock lock{handlers_mutex};
        handler = handlers[config].lock();
        if (!handler) {
            handler = std::make_shared<LiveCameraHandler>(config);
            handlers[config] = handler;
        }
    }
    return std::make_unique<LiveCamera>(std::move(handler), flip);
}

} // namespace UWP
