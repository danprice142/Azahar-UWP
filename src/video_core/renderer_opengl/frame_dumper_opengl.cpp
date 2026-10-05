// Copyright 2020-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <glad/glad.h>

#include <chrono>
#include <utility>

#include "core/core.h"
#include "core/dumping/backend.h"
#include "core/frontend/emu_window.h"
#include "video_core/renderer_opengl/frame_dumper_opengl.h"
#include "video_core/renderer_opengl/gl_texture_mailbox.h"

namespace OpenGL {

FrameDumperOpenGL::FrameDumperOpenGL(Core::System& system_, Frontend::EmuWindow& emu_window,
                                     BackendGetter backend_getter_)
    : system{system_}, backend_getter{std::move(backend_getter_)},
      context{emu_window.CreateSharedContext()} {
    if (!backend_getter) {
        backend_getter = [this] { return system.GetVideoDumper(); };
    }
}

FrameDumperOpenGL::~FrameDumperOpenGL() = default;

bool FrameDumperOpenGL::IsDumping() const {
    auto video_dumper = backend_getter();
    return video_dumper && video_dumper->IsDumping();
}

Layout::FramebufferLayout FrameDumperOpenGL::GetLayout() const {
    auto video_dumper = backend_getter();
    return video_dumper ? video_dumper->GetLayout() : Layout::FramebufferLayout{};
}

void FrameDumperOpenGL::StartDumping() {
    if (present_thread.joinable()) {
        present_thread.join();
    }

    present_thread = std::jthread([this](std::stop_token stop_token) { PresentLoop(stop_token); });
}

void FrameDumperOpenGL::StopDumping() {
    present_thread.request_stop();
    if (present_thread.joinable()) {
        present_thread.join();
    }
}

void FrameDumperOpenGL::PresentLoop(std::stop_token stop_token) {
    const auto scope = context->Acquire();

    // Wait until the backend is visible and active, or a 0x0 layout may be cached.
    Layout::FramebufferLayout layout{};
    while (!stop_token.stop_requested()) {
        const auto video_dumper = backend_getter();
        if (video_dumper && video_dumper->IsDumping()) {
            layout = video_dumper->GetLayout();
            if (layout.width > 0 && layout.height > 0) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    if (stop_token.stop_requested()) {
        return;
    }
    InitializeOpenGLObjects(layout);
    bool pbo_primed = false;

    while (!stop_token.stop_requested()) {
        auto frame = mailbox->TryGetPresentFrame(200);
        if (!frame) {
            continue;
        }

        if (frame->color_reloaded) {
            LOG_DEBUG(Render_OpenGL, "Reloading present frame");
            mailbox->ReloadPresentFrame(frame, layout.width, layout.height);
        }
        glWaitSync(frame->render_fence, 0, GL_TIMEOUT_IGNORED);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, frame->present.handle);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[current_pbo].handle);
        glReadPixels(0, 0, layout.width, layout.height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, 0);

        // Insert fence for the main thread to block on
        frame->present_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();

#ifdef ENABLE_FFMPEG
        auto video_dumper = backend_getter();
        if (video_dumper && pbo_primed) {
            // Bind the previous PBO and read the pixels
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[next_pbo].handle);
            GLubyte* pixels =
                static_cast<GLubyte*>(glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY));
            if (pixels) {
                VideoDumper::VideoFrame frame_data{layout.width, layout.height, pixels};
                video_dumper->AddVideoFrame(std::move(frame_data));
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            } else {
                LOG_ERROR(Render_OpenGL, "Could not map video dumping pixel buffer");
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }
#endif

        current_pbo = (current_pbo + 1) % 2;
        next_pbo = (current_pbo + 1) % 2;
        pbo_primed = true;
    }

    CleanupOpenGLObjects();
}

void FrameDumperOpenGL::InitializeOpenGLObjects(const Layout::FramebufferLayout& layout) {
    current_pbo = 1;
    next_pbo = 0;
    for (auto& buffer : pbos) {
        buffer.Create();
        glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer.handle);
        glBufferData(GL_PIXEL_PACK_BUFFER, layout.width * layout.height * 4, nullptr,
                     GL_STREAM_READ);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
}

void FrameDumperOpenGL::CleanupOpenGLObjects() {
    for (auto& buffer : pbos) {
        buffer.Release();
    }
}

} // namespace OpenGL
