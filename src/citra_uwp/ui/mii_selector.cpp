// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <utility>

#include <imgui.h>

#include "citra_uwp/ui/mii_selector.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/hle/applets/mii_selector.h"

namespace UWP {

void MiiSelectorUwp::Setup(const Frontend::MiiSelectorConfig& config_) {
    MiiSelector::Setup(config_);

    miis.clear();
    names.clear();
    miis.push_back(HLE::Applets::MiiSelector::GetStandardMiiResult().selected_mii_data);
    names.emplace_back("Standard Mii");
    for (auto& mii : Frontend::LoadMiis()) {
        names.push_back(Common::UTF16BufferToUTF8(mii.mii_name));
        miis.push_back(std::move(mii));
    }

    selected = std::clamp(static_cast<int>(config.initially_selected_mii_index), 0,
                          static_cast<int>(miis.size()) - 1);
    popup_pending.store(true, std::memory_order_relaxed);
    open.store(true, std::memory_order_release);
}

void MiiSelectorUwp::Draw() {
    if (!open.load(std::memory_order_acquire)) {
        return;
    }
    if (popup_pending.exchange(false, std::memory_order_acq_rel)) {
        ImGui::OpenPopup("Mii Selector");
    }
    if (!ImGui::BeginPopupModal("Mii Selector", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    if (!config.title.empty()) {
        ImGui::TextWrapped("%s", config.title.c_str());
        ImGui::Separator();
    }
    if (ImGui::BeginChild("##miis", ImVec2(520.0f, 360.0f), ImGuiChildFlags_Borders)) {
        for (int index = 0; index < static_cast<int>(names.size()); ++index) {
            ImGui::PushID(index);
            if (ImGui::Selectable(names[index].c_str(), selected == index)) {
                selected = index;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (config.enable_cancel_button) {
        if (ImGui::Button(Frontend::MII_BUTTON_CANCEL, ImVec2(160.0f, 0.0f)) ||
            (!ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
            Finalize(1, {});
            open.store(false, std::memory_order_release);
            ImGui::CloseCurrentPopup();
            LOG_INFO(Frontend, "Mii Selector cancelled");
        }
        ImGui::SameLine();
    }
    if (ImGui::Button(Frontend::MII_BUTTON_OKAY, ImVec2(160.0f, 0.0f))) {
        Finalize(0, miis[static_cast<std::size_t>(selected)]);
        open.store(false, std::memory_order_release);
        ImGui::CloseCurrentPopup();
        LOG_INFO(Frontend, "Mii Selector chose entry {}", selected);
    }

    ImGui::EndPopup();
}

} // namespace UWP
