// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Settings {
struct InputProfile;
}

namespace UWP::KeyboardInput {

/// Key modifiers recognised in hotkey chords.
enum Modifier : u16 {
    Ctrl = 1 << 0,
    Shift = 1 << 1,
    Alt = 1 << 2,
};

struct KeyPress {
    s32 key = 0;
    u16 modifiers = 0;
};

/// Up, down, left, right and modifier keys for an analog binding, as in the desktop frontend.
struct AnalogKeys {
    std::array<s32, 5> keys{};
    float modifier_scale = 0.5f;
};

/// Registers the engine that combines a keyboard key with another input binding.
void Init();
void Shutdown();

/// Splits a profile binding into its controller binding and keyboard key.
std::string ButtonBinding(const std::string& slot);
s32 ButtonKey(const std::string& slot);
std::string JoinButton(const std::string& binding, s32 key);
void SetButtonBinding(std::string& slot, const std::string& binding);
void SetButtonKey(std::string& slot, s32 key);

std::string AnalogBinding(const std::string& slot);
AnalogKeys AnalogKeysOf(const std::string& slot);
std::string JoinAnalog(const std::string& binding, const AnalogKeys& keys);
void SetAnalogBinding(std::string& slot, const std::string& binding);
void SetAnalogKeys(std::string& slot, const AnalogKeys& keys);

/// Adds the desktop frontend's default keys; existing keys are kept unless overwrite is set.
void ApplyDefaultKeys(Settings::InputProfile& profile, bool overwrite);

std::string KeyName(s32 key);
bool IsModifierKey(s32 key);
u16 ModifiersFromSdl(u16 sdl_modifiers);

/// Hotkey chords use the desktop frontend's text form, e.g. "Ctrl+P".
std::optional<KeyPress> ParseChord(std::string_view text);
std::string ChordText(const KeyPress& press);
bool ChordMatches(std::string_view chord, const KeyPress& press);

/// Key presses delivered since the last event pump.
void BeginKeyPresses();
void AddKeyPress(const KeyPress& press);
const std::vector<KeyPress>& KeyPresses();

/// Set while the configuration dialog captures a key, so the key does not drive the UI.
void SetKeyCaptureActive(bool active);
bool IsKeyCaptureActive();

} // namespace UWP::KeyboardInput
