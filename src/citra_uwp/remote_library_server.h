// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.

#pragma once

#include <functional>
#include <memory>

#include "citra_uwp/ui/game_list.h"
#include "common/common_types.h"

namespace UWP {

/// Read-only HTTP server for library metadata and ScreenScraper media, on the DSU port plus one.
class RemoteLibraryServer {
public:
    using SnapshotProvider = std::function<RemoteGameListSnapshot()>;

    RemoteLibraryServer();
    ~RemoteLibraryServer();

    RemoteLibraryServer(const RemoteLibraryServer&) = delete;
    RemoteLibraryServer& operator=(const RemoteLibraryServer&) = delete;

    bool Start(u16 port, SnapshotProvider provider);
    void Stop();
    [[nodiscard]] bool IsRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace UWP
