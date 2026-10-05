// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include "core/core.h"
#include "video_core/rasterizer_interface.h"

namespace Frontend {
class GraphicsContext;
}

namespace UWP {

/**
 * Owns the GL context and thread on which all core work runs. RunExclusive() executes a command
 * between RunLoop calls; commands that leave the core unpowered must be preceded by
 * PauseAndWait().
 */
class EmulationThread final {
public:
    struct Error {
        Core::System::ResultStatus status = Core::System::ResultStatus::ErrorUnknown;
        std::string details;
    };

    struct LoadProgress {
        VideoCore::LoadCallbackStage stage = VideoCore::LoadCallbackStage::Prepare;
        std::size_t value = 0;
        std::size_t total = 0;
        std::string object;
        bool switching_resources = false;
    };

    EmulationThread(Core::System& system_, std::unique_ptr<Frontend::GraphicsContext> context_);
    ~EmulationThread();

    EmulationThread(const EmulationThread&) = delete;
    EmulationThread& operator=(const EmulationThread&) = delete;
    EmulationThread(EmulationThread&&) = delete;
    EmulationThread& operator=(EmulationThread&&) = delete;

    /// Starts the worker paused.
    [[nodiscard]] bool Start(bool start_running = false);

    /// Asks the worker to stop after draining already queued exclusive commands.
    void RequestStop();

    /// Stops and joins the worker; System::Shutdown runs on the worker before its context is
    /// released.
    void Stop();
    void Join();

    void Pause();
    void Resume();

    /// Pauses and waits for an exclusive boundary, cancelling any in-flight resource
    /// preparation.
    void PauseAndWait();

    /// Waits for an exclusive boundary without changing the requested run state.
    void Synchronize();

    [[nodiscard]] bool IsAlive() const;
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] bool IsReady() const;
    [[nodiscard]] bool IsWorkerThread() const;

    /// Executes exactly one guest CPU instruction while remaining paused.
    void RequestSingleStep();

    /// Executes until the renderer publishes one additional frame while remaining paused.
    void RequestFrameAdvance();

    /// Queues custom texture and shader-cache loading; runs before the next RunLoop call.
    [[nodiscard]] bool RequestPrepareLoadedTitle();

    [[nodiscard]] std::optional<Error> TakeError();
    [[nodiscard]] std::optional<LoadProgress> TakeLoadProgress();

    /// Runs a command on the worker between RunLoop calls, or on the caller if no worker is
    /// active.
    template <typename Function>
    auto RunExclusive(Function&& function) -> std::invoke_result_t<std::decay_t<Function>&> {
        using FunctionType = std::decay_t<Function>;
        using ResultType = std::invoke_result_t<FunctionType&>;

        if (IsWorkerThread()) {
            if constexpr (std::is_void_v<ResultType>) {
                std::invoke(std::forward<Function>(function));
                return;
            } else {
                return std::invoke(std::forward<Function>(function));
            }
        }

        auto task = std::make_shared<std::packaged_task<ResultType()>>(
            FunctionType{std::forward<Function>(function)});
        auto result = task->get_future();

        if (!QueueExclusive([task] { (*task)(); })) {
            // QueueExclusive only rejects after the worker stopped accepting commands; wait for
            // its exit.
            WaitForExit();
            (*task)();
        }

        if constexpr (std::is_void_v<ResultType>) {
            result.get();
            return;
        } else {
            return result.get();
        }
    }

    /// Queues a fire-and-forget exclusive command. Returns false when no worker is active.
    [[nodiscard]] bool PostExclusive(std::function<void()> command);

private:
    enum class Action {
        Command,
        PrepareResources,
        Run,
        SingleStep,
        FrameAdvance,
        Stop,
    };

    [[nodiscard]] bool QueueExclusive(std::function<void()> command);
    void WaitForExit();
    void ThreadMain();
    void RunAction(Action action, std::function<void()> command);
    [[nodiscard]] bool PrepareLoadedTitle();
    void HandleResult(Core::System::ResultStatus status);
    void ReportError(Core::System::ResultStatus status, std::string details);
    void ReportException(const char* operation, const std::exception& exception);
    void PushLoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total,
                          const std::string& object, bool switching_resources);

    Core::System& system;
    std::unique_ptr<Frontend::GraphicsContext> context;

    mutable std::mutex state_mutex;
    std::condition_variable state_cv;
    std::thread worker_thread;
    std::thread::id worker_thread_id{};
    std::deque<std::function<void()>> commands;

    bool worker_alive = false;
    bool accepting_commands = false;
    bool running = false;
    bool prepare_resources_requested = false;
    bool resources_ready = false;
    std::size_t pending_single_steps = 0;
    std::size_t pending_frame_advances = 0;
    std::optional<u64> frame_advance_start;
    std::atomic_bool stop_requested{false};
    std::atomic_bool resource_loading_cancelled{false};

    mutable std::mutex output_mutex;
    std::deque<Error> errors;
    std::deque<LoadProgress> load_progress;
};

} // namespace UWP
