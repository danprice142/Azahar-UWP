// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <memory>
#include <tuple>

#include <SDL.h>

#include "citra_uwp/keyboard_input.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "core/frontend/input.h"
#include "input_common/main.h"

namespace UWP::KeyboardInput {

namespace {

constexpr const char* Engine = "keyboard_addon";
constexpr std::array<const char*, 5> AnalogDirections{"up", "down", "left", "right", "modifier"};

// Desktop frontend defaults, in NativeButton order.
constexpr std::array<s32, Settings::NativeButton::NumButtons> DefaultButtons{
    SDLK_a, SDLK_s, SDLK_z, SDLK_x, SDLK_t, SDLK_g, SDLK_f, SDLK_h, SDLK_q,
    SDLK_w, SDLK_m, SDLK_n, SDLK_o, SDLK_p, SDLK_1, SDLK_2, SDLK_b, SDLK_v,
};
constexpr std::array<std::array<s32, 5>, Settings::NativeAnalog::NumAnalogs> DefaultAnalogs{{
    {SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_d},
    {SDLK_i, SDLK_k, SDLK_j, SDLK_l, SDLK_d},
}};

std::vector<KeyPress> key_presses;
bool key_capture_active = false;

class AddonButton final : public Input::ButtonDevice {
public:
    AddonButton(std::unique_ptr<Input::ButtonDevice> binding_,
                std::unique_ptr<Input::ButtonDevice> key_)
        : binding(std::move(binding_)), key(std::move(key_)) {}

    bool GetStatus() const override {
        return (binding && binding->GetStatus()) || (key && key->GetStatus());
    }

private:
    std::unique_ptr<Input::ButtonDevice> binding;
    std::unique_ptr<Input::ButtonDevice> key;
};

class AddonAnalog final : public Input::AnalogDevice {
public:
    AddonAnalog(std::unique_ptr<Input::AnalogDevice> binding_,
                std::unique_ptr<Input::AnalogDevice> keys_)
        : binding(std::move(binding_)), keys(std::move(keys_)) {}

    std::tuple<float, float> GetStatus() const override {
        const auto binding_value = binding ? binding->GetStatus() : std::tuple<float, float>{};
        const auto keys_value = keys ? keys->GetStatus() : std::tuple<float, float>{};
        const auto magnitude_squared = [](const std::tuple<float, float>& value) {
            const auto [x, y] = value;
            return x * x + y * y;
        };
        return magnitude_squared(keys_value) > magnitude_squared(binding_value) ? keys_value
                                                                                : binding_value;
    }

private:
    std::unique_ptr<Input::AnalogDevice> binding;
    std::unique_ptr<Input::AnalogDevice> keys;
};

class AddonButtonFactory final : public Input::Factory<Input::ButtonDevice> {
public:
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override {
        const std::string binding = params.Get("binding", "");
        const int code = params.Get("code", 0);
        return std::make_unique<AddonButton>(
            binding.empty() ? nullptr : Input::CreateDevice<Input::ButtonDevice>(binding),
            code == 0 ? nullptr
                      : Input::CreateDevice<Input::ButtonDevice>(
                            InputCommon::GenerateKeyboardParam(code)));
    }
};

class AddonAnalogFactory final : public Input::Factory<Input::AnalogDevice> {
public:
    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage& params) override {
        const std::string binding = params.Get("binding", "");
        const AnalogKeys keys = AnalogKeysOf(params.Serialize());
        const bool has_keys =
            std::any_of(keys.keys.begin(), keys.keys.begin() + 4, [](s32 key) { return key != 0; });
        return std::make_unique<AddonAnalog>(
            binding.empty() ? nullptr : Input::CreateDevice<Input::AnalogDevice>(binding),
            has_keys
                ? Input::CreateDevice<Input::AnalogDevice>(InputCommon::GenerateAnalogParamFromKeys(
                      keys.keys[0], keys.keys[1], keys.keys[2], keys.keys[3], keys.keys[4],
                      keys.modifier_scale))
                : nullptr);
    }
};

bool IsAddon(const Common::ParamPackage& params) {
    return params.Get("engine", "") == Engine;
}

s32 NormalizeChordKey(s32 key) {
    switch (key) {
    case SDLK_KP_PLUS:
    case SDLK_EQUALS:
        return SDLK_PLUS;
    case SDLK_KP_MINUS:
        return SDLK_MINUS;
    case SDLK_KP_ENTER:
        return SDLK_RETURN;
    default:
        return key;
    }
}

} // namespace

void Init() {
    Input::RegisterFactory<Input::ButtonDevice>(Engine, std::make_shared<AddonButtonFactory>());
    Input::RegisterFactory<Input::AnalogDevice>(Engine, std::make_shared<AddonAnalogFactory>());
}

void Shutdown() {
    Input::UnregisterFactory<Input::ButtonDevice>(Engine);
    Input::UnregisterFactory<Input::AnalogDevice>(Engine);
}

std::string ButtonBinding(const std::string& slot) {
    const Common::ParamPackage params{slot};
    return IsAddon(params) ? params.Get("binding", "") : slot;
}

s32 ButtonKey(const std::string& slot) {
    const Common::ParamPackage params{slot};
    return IsAddon(params) ? params.Get("code", 0) : 0;
}

std::string JoinButton(const std::string& binding, s32 key) {
    if (key == 0) {
        return binding;
    }
    Common::ParamPackage params{{"engine", Engine}};
    params.Set("code", key);
    if (!binding.empty()) {
        params.Set("binding", binding);
    }
    return params.Serialize();
}

void SetButtonBinding(std::string& slot, const std::string& binding) {
    slot = JoinButton(binding, ButtonKey(slot));
}

void SetButtonKey(std::string& slot, s32 key) {
    slot = JoinButton(ButtonBinding(slot), key);
}

std::string AnalogBinding(const std::string& slot) {
    return ButtonBinding(slot);
}

AnalogKeys AnalogKeysOf(const std::string& slot) {
    const Common::ParamPackage params{slot};
    AnalogKeys keys;
    if (!IsAddon(params)) {
        return keys;
    }
    for (std::size_t i = 0; i < AnalogDirections.size(); ++i) {
        keys.keys[i] = params.Get(AnalogDirections[i], 0);
    }
    keys.modifier_scale = params.Get("modifier_scale", 0.5f);
    return keys;
}

std::string JoinAnalog(const std::string& binding, const AnalogKeys& keys) {
    if (std::all_of(keys.keys.begin(), keys.keys.begin() + 4, [](s32 key) { return key == 0; })) {
        return binding;
    }
    Common::ParamPackage params{{"engine", Engine}};
    for (std::size_t i = 0; i < AnalogDirections.size(); ++i) {
        params.Set(AnalogDirections[i], keys.keys[i]);
    }
    params.Set("modifier_scale", keys.modifier_scale);
    if (!binding.empty()) {
        params.Set("binding", binding);
    }
    return params.Serialize();
}

void SetAnalogBinding(std::string& slot, const std::string& binding) {
    slot = JoinAnalog(binding, AnalogKeysOf(slot));
}

void SetAnalogKeys(std::string& slot, const AnalogKeys& keys) {
    slot = JoinAnalog(AnalogBinding(slot), keys);
}

void ApplyDefaultKeys(Settings::InputProfile& profile, bool overwrite) {
    for (std::size_t i = 0; i < DefaultButtons.size(); ++i) {
        if (overwrite || ButtonKey(profile.buttons[i]) == 0) {
            SetButtonKey(profile.buttons[i], DefaultButtons[i]);
        }
    }
    for (std::size_t i = 0; i < DefaultAnalogs.size(); ++i) {
        const AnalogKeys current = AnalogKeysOf(profile.analogs[i]);
        if (overwrite || std::all_of(current.keys.begin(), current.keys.begin() + 4,
                                     [](s32 key) { return key == 0; })) {
            SetAnalogKeys(profile.analogs[i], AnalogKeys{DefaultAnalogs[i], 0.5f});
        }
    }
}

std::string KeyName(s32 key) {
    if (key == 0) {
        return {};
    }
    const char* name = SDL_GetKeyName(static_cast<SDL_Keycode>(key));
    return name != nullptr && name[0] != '\0' ? std::string{name} : "Key " + std::to_string(key);
}

bool IsModifierKey(s32 key) {
    switch (key) {
    case SDLK_LCTRL:
    case SDLK_RCTRL:
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
    case SDLK_LALT:
    case SDLK_RALT:
    case SDLK_LGUI:
    case SDLK_RGUI:
        return true;
    default:
        return false;
    }
}

u16 ModifiersFromSdl(u16 sdl_modifiers) {
    u16 modifiers = 0;
    if ((sdl_modifiers & KMOD_CTRL) != 0) {
        modifiers |= Ctrl;
    }
    if ((sdl_modifiers & KMOD_SHIFT) != 0) {
        modifiers |= Shift;
    }
    if ((sdl_modifiers & KMOD_ALT) != 0) {
        modifiers |= Alt;
    }
    return modifiers;
}

std::optional<KeyPress> ParseChord(std::string_view text) {
    KeyPress press;
    constexpr std::array<std::pair<std::string_view, Modifier>, 3> prefixes{{
        {"Ctrl+", Ctrl},
        {"Shift+", Shift},
        {"Alt+", Alt},
    }};
    bool matched = true;
    while (matched) {
        matched = false;
        for (const auto& [prefix, modifier] : prefixes) {
            if (text.size() > prefix.size() && text.substr(0, prefix.size()) == prefix) {
                press.modifiers |= modifier;
                text.remove_prefix(prefix.size());
                matched = true;
            }
        }
    }
    if (text.empty()) {
        return std::nullopt;
    }
    press.key = SDL_GetKeyFromName(std::string{text}.c_str());
    if (press.key == SDLK_UNKNOWN || IsModifierKey(press.key)) {
        return std::nullopt;
    }
    return press;
}

std::string ChordText(const KeyPress& press) {
    std::string text;
    if ((press.modifiers & Ctrl) != 0) {
        text += "Ctrl+";
    }
    if ((press.modifiers & Shift) != 0) {
        text += "Shift+";
    }
    if ((press.modifiers & Alt) != 0) {
        text += "Alt+";
    }
    return text + KeyName(press.key);
}

bool ChordMatches(std::string_view chord, const KeyPress& press) {
    const auto parsed = ParseChord(chord);
    if (!parsed) {
        return false;
    }
    const s32 key = NormalizeChordKey(parsed->key);
    if (key != NormalizeChordKey(press.key)) {
        return false;
    }
    // "+" is a shifted key on most layouts, so Shift is not part of its chord.
    const u16 ignored = key == SDLK_PLUS ? Shift : 0;
    return (parsed->modifiers & ~ignored) == (press.modifiers & ~ignored);
}

void BeginKeyPresses() {
    key_presses.clear();
}

void AddKeyPress(const KeyPress& press) {
    key_presses.push_back(press);
}

const std::vector<KeyPress>& KeyPresses() {
    return key_presses;
}

void SetKeyCaptureActive(bool active) {
    key_capture_active = active;
}

bool IsKeyCaptureActive() {
    return key_capture_active;
}

} // namespace UWP::KeyboardInput
