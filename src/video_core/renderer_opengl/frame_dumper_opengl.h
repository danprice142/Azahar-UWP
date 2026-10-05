// Copyright 2020 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include "common/polyfill_thread.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/renderer_opengl/gl_resource_manager.h"

namespace Frontend {
class EmuWindow;
class GraphicsContext;
class TextureMailbox;
} // namespace Frontend

namespace Core {
class System;
}

namespace VideoDumper {
class Backend;
}

namespace OpenGL {

class RendererOpenGL;

/**
 * This is the 'presentation' part in frame dumping.
 * Processes frames/textures sent to its mailbox, downloads the pixels and sends the data
 * to the video encoding backend.
 */
class FrameDumperOpenGL {
public:
    using BackendGetter = std::function<std::shared_ptr<VideoDumper::Backend>()>;
    explicit FrameDumperOpenGL(Core::System& system, Frontend::EmuWindow& emu_window,
                               BackendGetter backend_getter = {});
    ~FrameDumperOpenGL();

    bool IsDumping() const;
    Layout::FramebufferLayout GetLayout() const;
    void StartDumping();
    void StopDumping();

    std::unique_ptr<Frontend::TextureMailbox> mailbox;

private:
    void InitializeOpenGLObjects(const Layout::FramebufferLayout& layout);
    void CleanupOpenGLObjects();
    void PresentLoop(std::stop_token stop_token);

private:
    Core::System& system;
    BackendGetter backend_getter;
    std::unique_ptr<Frontend::GraphicsContext> context;
    std::jthread present_thread;

    // PBOs used to dump frames faster
    std::array<OGLBuffer, 2> pbos;
    GLuint current_pbo = 1;
    GLuint next_pbo = 0;
};

} // namespace OpenGL
