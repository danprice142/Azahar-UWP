// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "citra_uwp/emulation_thread.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "common/logging/log.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/frontend/emu_window.h"
#include "core/loader/loader.h"
#include "video_core/custom_textures/custom_tex_manager.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

namespace UWP {

namespace {

constexpr std::size_t MaxQueuedErrors = 16;
constexpr std::size_t MaxQueuedProgressUpdates = 512;

} // namespace

EmulationThread::EmulationThread(Core::System& system_,
                                 std::unique_ptr<Frontend::GraphicsContext> context_)
    : system{system_}, context{std::move(context_)} {}

EmulationThread::~EmulationThread() {
    Stop();
}

bool EmulationThread::Start(bool start_running) {
    if (context == nullptr) {
        LOG_ERROR(Frontend, "Cannot start UWP emulation thread without a shared graphics context");
        return false;
    }

    {
        std::scoped_lock lock{state_mutex};
        if (worker_thread.joinable() || worker_alive) {
            LOG_WARNING(Frontend, "UWP emulation thread is already active or has not been joined");
            return false;
        }

        stop_requested.store(false, std::memory_order_release);
        worker_alive = true;
        accepting_commands = true;
        running = start_running;
        prepare_resources_requested = false;
        resources_ready = false;
        pending_single_steps = 0;
        pending_frame_advances = 0;
        frame_advance_start.reset();
        commands.clear();
    }

    {
        std::scoped_lock lock{output_mutex};
        errors.clear();
        load_progress.clear();
    }

    try {
        worker_thread = std::thread{&EmulationThread::ThreadMain, this};
    } catch (const std::exception& exception) {
        std::scoped_lock lock{state_mutex};
        worker_alive = false;
        accepting_commands = false;
        running = false;
        LOG_ERROR(Frontend, "Failed to create UWP emulation thread: {}", exception.what());
        return false;
    }

    return true;
}

void EmulationThread::RequestStop() {
    // The worker implements pause and frame advance itself; the core limiter would block RunLoop.
    system.frame_limiter.SetFrameAdvancing(false);
    resource_loading_cancelled.store(true, std::memory_order_release);

    bool shutdown_on_caller = false;
    {
        std::scoped_lock lock{state_mutex};
        stop_requested.store(true, std::memory_order_release);
        running = false;
        pending_single_steps = 0;
        pending_frame_advances = 0;
        frame_advance_start.reset();
        shutdown_on_caller = !worker_alive;
    }
    state_cv.notify_all();

    if (shutdown_on_caller && system.IsPoweredOn()) {
        system.Shutdown();
    }
}

void EmulationThread::Stop() {
    if (IsWorkerThread()) {
        RequestStop();
        return;
    }

    RequestStop();
    Join();
}

void EmulationThread::Join() {
    if (!worker_thread.joinable()) {
        return;
    }
    if (worker_thread.get_id() == std::this_thread::get_id()) {
        LOG_ERROR(Frontend, "UWP emulation thread cannot join itself");
        return;
    }
    worker_thread.join();
}

void EmulationThread::Pause() {
    {
        std::scoped_lock lock{state_mutex};
        running = false;
        pending_single_steps = 0;
        pending_frame_advances = 0;
        frame_advance_start.reset();
    }
    state_cv.notify_all();
}

void EmulationThread::Resume() {
    {
        std::scoped_lock lock{state_mutex};
        running = true;
        pending_single_steps = 0;
        pending_frame_advances = 0;
        frame_advance_start.reset();
    }
    state_cv.notify_all();
}

void EmulationThread::PauseAndWait() {
    // Shutdown/restart must not wait for a long custom-texture or shader-cache preload.
    resource_loading_cancelled.store(true, std::memory_order_release);
    Pause();
    Synchronize();
}

void EmulationThread::Synchronize() {
    if (IsAlive() && !IsWorkerThread()) {
        RunExclusive([] {});
    }
}

bool EmulationThread::IsAlive() const {
    std::scoped_lock lock{state_mutex};
    return worker_alive;
}

bool EmulationThread::IsRunning() const {
    std::scoped_lock lock{state_mutex};
    return worker_alive && running;
}

bool EmulationThread::IsReady() const {
    std::scoped_lock lock{state_mutex};
    return worker_alive && resources_ready;
}

bool EmulationThread::IsWorkerThread() const {
    std::scoped_lock lock{state_mutex};
    return worker_alive && worker_thread_id == std::this_thread::get_id();
}

void EmulationThread::RequestSingleStep() {
    {
        std::scoped_lock lock{state_mutex};
        if (!worker_alive) {
            return;
        }
        running = false;
        pending_frame_advances = 0;
        frame_advance_start.reset();
        ++pending_single_steps;
    }
    state_cv.notify_all();
}

void EmulationThread::RequestFrameAdvance() {
    {
        std::scoped_lock lock{state_mutex};
        if (!worker_alive) {
            return;
        }
        running = false;
        pending_single_steps = 0;
        ++pending_frame_advances;
    }
    state_cv.notify_all();
}

bool EmulationThread::RequestPrepareLoadedTitle() {
    {
        std::scoped_lock lock{state_mutex};
        if (!worker_alive || stop_requested.load(std::memory_order_acquire)) {
            return false;
        }
        prepare_resources_requested = true;
        resources_ready = false;
        resource_loading_cancelled.store(false, std::memory_order_release);
    }
    state_cv.notify_all();
    return true;
}

std::optional<EmulationThread::Error> EmulationThread::TakeError() {
    std::scoped_lock lock{output_mutex};
    if (errors.empty()) {
        return std::nullopt;
    }
    Error error = std::move(errors.front());
    errors.pop_front();
    return error;
}

std::optional<EmulationThread::LoadProgress> EmulationThread::TakeLoadProgress() {
    std::scoped_lock lock{output_mutex};
    if (load_progress.empty()) {
        return std::nullopt;
    }
    LoadProgress progress = std::move(load_progress.front());
    load_progress.pop_front();
    return progress;
}

bool EmulationThread::PostExclusive(std::function<void()> command) {
    return QueueExclusive(std::move(command));
}

bool EmulationThread::QueueExclusive(std::function<void()> command) {
    if (!command) {
        return false;
    }

    {
        std::scoped_lock lock{state_mutex};
        if (!accepting_commands || !worker_alive) {
            return false;
        }
        commands.emplace_back(std::move(command));
    }
    state_cv.notify_all();
    return true;
}

void EmulationThread::WaitForExit() {
    std::unique_lock lock{state_mutex};
    state_cv.wait(lock, [this] { return !worker_alive; });
}

void EmulationThread::ThreadMain() {
    Common::SetCurrentThreadName("Azahar UWP Emulation");
    {
        const auto context_scope = context->Acquire();

        system.frame_limiter.SetFrameAdvancing(false);

        {
            std::scoped_lock lock{state_mutex};
            worker_thread_id = std::this_thread::get_id();
            prepare_resources_requested = system.IsPoweredOn();
            resources_ready = !prepare_resources_requested;
            resource_loading_cancelled.store(false, std::memory_order_release);
        }
        state_cv.notify_all();

        for (;;) {
            Action action = Action::Stop;
            std::function<void()> command;

            {
                std::unique_lock lock{state_mutex};
                state_cv.wait(lock, [this] {
                    return !commands.empty() || stop_requested.load(std::memory_order_acquire) ||
                           prepare_resources_requested || running || pending_single_steps != 0 ||
                           pending_frame_advances != 0 || frame_advance_start.has_value();
                });

                if (!commands.empty()) {
                    command = std::move(commands.front());
                    commands.pop_front();
                    action = Action::Command;
                } else if (stop_requested.load(std::memory_order_acquire)) {
                    // QueueExclusive observes this under the same lock, so no command can be
                    // stranded.
                    accepting_commands = false;
                    action = Action::Stop;
                } else if (prepare_resources_requested) {
                    prepare_resources_requested = false;
                    resources_ready = false;
                    action = Action::PrepareResources;
                } else if (pending_single_steps != 0) {
                    --pending_single_steps;
                    action = Action::SingleStep;
                } else if (frame_advance_start.has_value() || pending_frame_advances != 0) {
                    if (!frame_advance_start.has_value()) {
                        --pending_frame_advances;
                        if (system.IsPoweredOn()) {
                            frame_advance_start = system.GPU().Renderer().GetCurrentFrame();
                        }
                    }
                    action = Action::FrameAdvance;
                } else if (running) {
                    action = Action::Run;
                }
            }

            if (action == Action::Stop) {
                try {
                    if (system.IsPoweredOn()) {
                        system.Shutdown();
                    }
                } catch (const std::exception& exception) {
                    ReportException("worker shutdown", exception);
                } catch (...) {
                    ReportError(Core::System::ResultStatus::ErrorUnknown,
                                "Unknown exception while shutting down the emulated system");
                }
                break;
            }
            RunAction(action, std::move(command));
        }

        // The callback is static because rasterizers are recreated on restart; clear it before
        // exit.
        VideoCore::RasterizerInterface::SetSwitchDiskResourcesCallback({});
    }

    {
        std::scoped_lock lock{state_mutex};
        worker_thread_id = {};
        running = false;
        resources_ready = false;
        accepting_commands = false;
        worker_alive = false;
    }
    state_cv.notify_all();
}

void EmulationThread::RunAction(Action action, std::function<void()> command) {
    try {
        switch (action) {
        case Action::Command:
            command();
            break;
        case Action::PrepareResources: {
            const bool prepared = PrepareLoadedTitle();
            std::scoped_lock lock{state_mutex};
            resources_ready = prepared && !prepare_resources_requested;
            break;
        }
        case Action::Run:
            HandleResult(system.RunLoop());
            break;
        case Action::SingleStep:
            if (!system.IsPoweredOn()) {
                HandleResult(Core::System::ResultStatus::ErrorNotInitialized);
            } else {
                HandleResult(system.SingleStep());
                std::this_thread::yield();
            }
            break;
        case Action::FrameAdvance: {
            std::optional<u64> frame_start;
            {
                std::scoped_lock lock{state_mutex};
                frame_start = frame_advance_start;
            }
            if (!system.IsPoweredOn() || !frame_start.has_value()) {
                {
                    std::scoped_lock lock{state_mutex};
                    frame_advance_start.reset();
                }
                HandleResult(Core::System::ResultStatus::ErrorNotInitialized);
                break;
            }

            HandleResult(system.RunLoop());
            if (system.IsPoweredOn()) {
                const u64 current_frame = system.GPU().Renderer().GetCurrentFrame();
                std::scoped_lock lock{state_mutex};
                if (frame_advance_start.has_value() && current_frame != frame_start.value()) {
                    frame_advance_start.reset();
                }
            }
            break;
        }
        case Action::Stop:
            break;
        }
    } catch (const std::exception& exception) {
        ReportException("UWP emulation worker action", exception);
        Pause();
    } catch (...) {
        ReportError(Core::System::ResultStatus::ErrorUnknown,
                    "Unknown exception in UWP emulation worker");
        Pause();
    }
}

bool EmulationThread::PrepareLoadedTitle() {
    if (!system.IsPoweredOn() || resource_loading_cancelled.load(std::memory_order_acquire)) {
        return false;
    }

    const auto progress_callback = [this](VideoCore::LoadCallbackStage stage, std::size_t value,
                                          std::size_t total, const std::string& object) {
        PushLoadProgress(stage, value, total, object, false);
    };

    if (Settings::values.custom_textures && Settings::values.preload_textures) {
        PushLoadProgress(VideoCore::LoadCallbackStage::Preload, 0, 0, "", false);
        system.CustomTexManager().PreloadTextures(resource_loading_cancelled, progress_callback);
    }

    if (resource_loading_cancelled.load(std::memory_order_acquire)) {
        return false;
    }

    VideoCore::RasterizerInterface::SetSwitchDiskResourcesCallback(
        [this](VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total,
               const std::string& object) { PushLoadProgress(stage, value, total, object, true); });

    PushLoadProgress(VideoCore::LoadCallbackStage::Prepare, 0, 0, "", false);

    u64 program_id{};
    system.GetAppLoader().ReadProgramId(program_id);
    system.GPU().ApplyPerProgramSettings(program_id);

    if (auto* rasterizer = system.GPU().Renderer().Rasterizer(); rasterizer != nullptr) {
        rasterizer->LoadDefaultDiskResources(resource_loading_cancelled, progress_callback);
    } else {
        ReportError(Core::System::ResultStatus::ErrorNotInitialized,
                    "Renderer has no rasterizer while loading disk resources");
        return false;
    }

    if (resource_loading_cancelled.load(std::memory_order_acquire)) {
        return false;
    }

    PushLoadProgress(VideoCore::LoadCallbackStage::Complete, 0, 0, "", false);
    return true;
}

void EmulationThread::HandleResult(Core::System::ResultStatus status) {
    if (status == Core::System::ResultStatus::Success) {
        return;
    }

    std::string details;
    if (status != Core::System::ResultStatus::ShutdownRequested) {
        details = system.GetStatusDetails();
    }
    ReportError(status, std::move(details));
    Pause();
}

void EmulationThread::ReportError(Core::System::ResultStatus status, std::string details) {
    std::scoped_lock lock{output_mutex};
    if (errors.size() == MaxQueuedErrors) {
        errors.pop_front();
    }
    errors.push_back(Error{status, std::move(details)});
}

void EmulationThread::ReportException(const char* operation, const std::exception& exception) {
    ReportError(Core::System::ResultStatus::ErrorUnknown,
                std::string{operation} + ": " + exception.what());
}

void EmulationThread::PushLoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value,
                                       std::size_t total, const std::string& object,
                                       bool switching_resources) {
    std::scoped_lock lock{output_mutex};
    if (load_progress.size() == MaxQueuedProgressUpdates) {
        load_progress.pop_front();
    }
    load_progress.push_back(LoadProgress{stage, value, total, object, switching_resources});
}

} // namespace UWP
