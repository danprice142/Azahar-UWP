// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>

#include "common/common_types.h"

namespace UWP {

/// Removes input bindings unsupported by the UWP frontend and configures its DSU devices.
void NormalizeInputSettings();

/// Replaces display layouts unsupported by the single-window UWP frontend.
void NormalizeDisplaySettings();

void LoadConfig();

void SaveConfig();

[[nodiscard]] std::string GetPerGameConfigPath(u64 title_id,
                                               const std::string& application_path = {});

/// Loads title-specific overrides after restoring every setting to its global slot.
bool LoadPerGameConfig(u64 title_id, const std::string& application_path = {});

bool SavePerGameConfig(u64 title_id, const std::string& application_path = {});

/// Returns true when the custom INI was absent or removed.
bool DeletePerGameConfig(u64 title_id, const std::string& application_path = {});

void RestoreGlobalConfig();

} // namespace UWP
