// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <sstream>
#include <string_view>

#include <fmt/format.h>
#include <imgui.h>

#include "citra_uwp/ui/dialogs.h"
#include "citra_uwp/ui/virtual_keyboard.h"
#include "citra_uwp/ui/widgets.h"
#include "common/common_paths.h"
#include "common/dynamic_library/ffmpeg.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/cheats/cheat_base.h"
#include "core/cheats/cheats.h"
#include "core/cheats/gateway_cheat.h"
#include "core/core.h"
#include "core/dumping/ffmpeg_backend.h"
#include "core/movie.h"

namespace UWP {

namespace {

using namespace Widgets;

template <std::size_t Size>
void SetTextBuffer(std::array<char, Size>& buffer, const std::string& text) {
    buffer.fill('\0');
    std::copy_n(text.begin(), std::min(text.size(), buffer.size() - 1), buffer.begin());
}

template <typename Choice>
int FindChoice(const std::vector<Choice>& choices, std::string_view configured,
               std::initializer_list<std::string_view> fallbacks) {
    const auto find = [&](std::string_view name) {
        return std::find_if(choices.begin(), choices.end(),
                            [name](const Choice& choice) { return choice.name == name; });
    };

    if (const auto configured_it = find(configured); configured_it != choices.end()) {
        return static_cast<int>(std::distance(choices.begin(), configured_it));
    }
    for (const std::string_view fallback : fallbacks) {
        if (const auto fallback_it = find(fallback); fallback_it != choices.end()) {
            return static_cast<int>(std::distance(choices.begin(), fallback_it));
        }
    }
    return 0;
}

template <typename Choice>
bool ChoiceCombo(const char* label, int& selected, const std::vector<Choice>& choices) {
    if (choices.empty()) {
        ImGui::BeginDisabled();
        ImGui::Button(fmt::format("{}: unavailable", label).c_str());
        ImGui::EndDisabled();
        return false;
    }

    selected = std::clamp(selected, 0, static_cast<int>(choices.size()) - 1);
    bool changed = false;
    if (ImGui::BeginCombo(label, choices[static_cast<std::size_t>(selected)].label.c_str())) {
        for (std::size_t i = 0; i < choices.size(); ++i) {
            const bool is_selected = selected == static_cast<int>(i);
            if (ImGui::Selectable(choices[i].label.c_str(), is_selected)) {
                selected = static_cast<int>(i);
                changed = true;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

std::string ResolveMoviePath(std::string_view requested_path, bool create_parent) {
    std::filesystem::path path{requested_path};
    if (!path.is_absolute()) {
        path = std::filesystem::path{FileUtil::GetUserPath(FileUtil::UserPath::UserDir)} /
               "movies" / path;
    }
    if (!path.has_extension()) {
        path.replace_extension(".ctm");
    }
    path = path.lexically_normal();

    if (create_parent) {
        const std::string parent = path.parent_path().string();
        if (!parent.empty() && !FileUtil::CreateFullPath(parent)) {
            LOG_WARNING(Movie, "Could not create movie output directory {}", parent);
        }
    }
    return path.string();
}

} // namespace

void MovieDialog::Draw(Core::System& system) {
    auto& movie = system.Movie();

    if (show_record) {
        SetNextWindowFullArea();
        if (ImGui::Begin("Record Movie", nullptr)) {
            HandleGamepadBack(show_record);
            GetVirtualKeyboard().InputText("Movie file", record_path.data(), record_path.size());
            Tooltip("Relative names are written under the active data root's movies folder.");
            GetVirtualKeyboard().InputText("Author", record_author.data(), record_author.size());

            ImGui::Separator();
            ImGui::BeginDisabled(record_path[0] == '\0');
            if (ImGui::Button("Record", ImVec2(140.0f, 0.0f))) {
                pending_record =
                    RecordRequest{ResolveMoviePath(record_path.data(), true), record_author.data()};
                show_record = false;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(140.0f, 0.0f))) {
                show_record = false;
            }
        }
        ImGui::End();
    }

    if (show_play) {
        SetNextWindowFullArea();
        if (ImGui::Begin("Play Movie", nullptr)) {
            HandleGamepadBack(show_play);
            if (GetVirtualKeyboard().InputText("Movie file", play_path.data(), play_path.size())) {
                validation_message.clear();
            }
            Tooltip("Relative names are read from the active data root's movies folder.");

            if (ImGui::Button("Validate")) {
                const std::string resolved_path = ResolveMoviePath(play_path.data(), false);
                switch (movie.ValidateMovie(resolved_path)) {
                case Core::Movie::ValidationResult::OK:
                    validation_message = "Movie is valid.";
                    break;
                case Core::Movie::ValidationResult::RevisionDismatch:
                    validation_message =
                        "Recorded with a different revision; playback may desynchronise.";
                    break;
                case Core::Movie::ValidationResult::InputCountDismatch:
                    validation_message =
                        "Input count differs from the recording; playback may desynchronise.";
                    break;
                case Core::Movie::ValidationResult::Invalid:
                default:
                    validation_message = "Not a valid movie file.";
                    break;
                }
            }
            if (!validation_message.empty()) {
                ImGui::TextWrapped("%s", validation_message.c_str());
            }

            ImGui::Separator();
            const std::string resolved_path =
                play_path[0] == '\0' ? std::string{} : ResolveMoviePath(play_path.data(), false);
            const auto validation = resolved_path.empty() ? Core::Movie::ValidationResult::Invalid
                                                          : movie.ValidateMovie(resolved_path);
            const bool can_play = validation == Core::Movie::ValidationResult::OK ||
                                  validation == Core::Movie::ValidationResult::RevisionDismatch;
            ImGui::BeginDisabled(!can_play);
            if (ImGui::Button("Play", ImVec2(140.0f, 0.0f))) {
                pending_playback = resolved_path;
                show_play = false;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(140.0f, 0.0f))) {
                show_play = false;
            }
        }
        ImGui::End();
    }
}

void MovieDialog::ToggleReadOnly(Core::System& system) {
    read_only = !read_only;
    system.Movie().SetReadOnly(read_only);
}

void MovieDialog::Close(Core::System& system) {
    system.Movie().Shutdown();
    recording = false;
    playing = false;
}

void MovieDialog::SaveWithoutClosing(Core::System& system) {
    // The Qt action flushes the movie to disk while leaving recording active.
    system.Movie().SaveMovie();
    LOG_INFO(Movie, "Movie saved without closing");
}

void DumpingDialog::Open() {
    show = true;
    show_options = false;
    pending_start.reset();
    pending_stop = false;
    if (!dumping) {
        status_message.clear();
        status_is_error = false;
    }
}

void DumpingDialog::LoadFFmpegChoices() {
    if (ffmpeg_checked) {
        return;
    }
    ffmpeg_checked = true;
    ffmpeg_available = false;
    formats.clear();

    if (!DynamicLibrary::FFmpeg::LoadFFmpeg()) {
        status_message =
            "The packaged FFmpeg 6 UWP libraries could not be loaded. See azahar_log.txt for the "
            "specific DLL or symbol error.";
        status_is_error = true;
        return;
    }

    const auto available_formats = VideoDumper::ListFormats();
    const auto video_encoders = VideoDumper::ListEncoders(AVMEDIA_TYPE_VIDEO);
    const auto audio_encoders = VideoDumper::ListEncoders(AVMEDIA_TYPE_AUDIO);

    for (const auto& format : available_formats) {
        FormatChoice choice;
        choice.name = format.name;
        choice.label = format.long_name.empty()
                           ? format.name
                           : fmt::format("{} ({})", format.long_name, format.name);
        choice.extensions = format.extensions;

        for (const auto& encoder : video_encoders) {
            if (format.supported_video_codecs.contains(encoder.codec)) {
                choice.video_encoders.push_back(
                    {encoder.name, encoder.long_name.empty()
                                       ? encoder.name
                                       : fmt::format("{} ({})", encoder.long_name, encoder.name)});
            }
        }
        for (const auto& encoder : audio_encoders) {
            if (format.supported_audio_codecs.contains(encoder.codec)) {
                choice.audio_encoders.push_back(
                    {encoder.name, encoder.long_name.empty()
                                       ? encoder.name
                                       : fmt::format("{} ({})", encoder.long_name, encoder.name)});
            }
        }

        // A container is usable only when a packaged encoder can supply each stream.
        if (!choice.video_encoders.empty() && !choice.audio_encoders.empty()) {
            formats.emplace_back(std::move(choice));
        }
    }

    if (formats.empty()) {
        status_message =
            "FFmpeg loaded, but it exposed no output format with both a video and audio encoder.";
        status_is_error = true;
        return;
    }

    ffmpeg_available = true;
    status_message = "FFmpeg 6 UWP runtime loaded.";
    status_is_error = false;
    SelectConfiguredValues();
}

void DumpingDialog::SelectConfiguredValues() {
    if (formats.empty()) {
        return;
    }

    auto& settings = Settings::values;
    format_index = FindChoice(formats, settings.output_format, {"mp4", "matroska", "avi"});
    format_index = std::clamp(format_index, 0, static_cast<int>(formats.size()) - 1);
    const auto& format = formats[static_cast<std::size_t>(format_index)];

    video_encoder_index = FindChoice(format.video_encoders, settings.video_encoder,
                                     {"h264_mf", "mpeg4", "ffv1", "rawvideo"});
    audio_encoder_index =
        FindChoice(format.audio_encoders, settings.audio_encoder, {"aac", "flac", "pcm_s16le"});

    const bool same_video_encoder =
        format.video_encoders[static_cast<std::size_t>(video_encoder_index)].name ==
        settings.video_encoder;
    const bool same_audio_encoder =
        format.audio_encoders[static_cast<std::size_t>(audio_encoder_index)].name ==
        settings.audio_encoder;
    const bool same_format = format.name == settings.output_format;
    SetTextBuffer(format_options, same_format ? settings.format_options : std::string{});
    SetTextBuffer(video_encoder_options,
                  same_video_encoder ? settings.video_encoder_options : std::string{});
    SetTextBuffer(audio_encoder_options,
                  same_audio_encoder ? settings.audio_encoder_options : std::string{});

    video_bitrate = static_cast<int>(
        std::min<u64>(settings.video_bitrate, static_cast<u64>(std::numeric_limits<int>::max())));
    audio_bitrate = static_cast<int>(
        std::min<u64>(settings.audio_bitrate, static_cast<u64>(std::numeric_limits<int>::max())));
    if (output_path[0] == '\0') {
        SetDefaultOutputPath();
    }
}

void DumpingDialog::SetDefaultOutputPath() {
    if (formats.empty()) {
        return;
    }

    const auto& format = formats[static_cast<std::size_t>(
        std::clamp(format_index, 0, static_cast<int>(formats.size()) - 1))];
    const std::string extension = !format.extensions.empty() && !format.extensions.front().empty()
                                      ? format.extensions.front()
                                      : format.name;
    const std::filesystem::path directory =
        std::filesystem::path{FileUtil::GetUserPath(FileUtil::UserPath::UserDir)} / "videos";

    std::filesystem::path candidate;
    for (unsigned index = 1; index < 10'000; ++index) {
        candidate = directory / fmt::format("Azahar-{:04}.{}", index, extension);
        if (!FileUtil::Exists(candidate.string())) {
            break;
        }
    }
    SetTextBuffer(output_path, candidate.string());
    path_is_default = true;
}

void DumpingDialog::ApplyConfiguration() {
    if (formats.empty()) {
        return;
    }

    const auto& format = formats[static_cast<std::size_t>(
        std::clamp(format_index, 0, static_cast<int>(formats.size()) - 1))];
    video_encoder_index =
        std::clamp(video_encoder_index, 0, static_cast<int>(format.video_encoders.size()) - 1);
    audio_encoder_index =
        std::clamp(audio_encoder_index, 0, static_cast<int>(format.audio_encoders.size()) - 1);

    auto& settings = Settings::values;
    settings.output_format = format.name;
    settings.format_options = format_options.data();
    settings.video_encoder =
        format.video_encoders[static_cast<std::size_t>(video_encoder_index)].name;
    settings.video_encoder_options = video_encoder_options.data();
    settings.video_bitrate = static_cast<u64>(std::max(video_bitrate, 0));
    settings.audio_encoder =
        format.audio_encoders[static_cast<std::size_t>(audio_encoder_index)].name;
    settings.audio_encoder_options = audio_encoder_options.data();
    settings.audio_bitrate = static_cast<u64>(std::max(audio_bitrate, 0));
}

void DumpingDialog::Draw(Core::System& system) {
    if (!show) {
        return;
    }

    const auto registered_dumper = system.GetVideoDumper();
    dumping = registered_dumper && registered_dumper->IsDumping();
    LoadFFmpegChoices();

    SetNextWindowFullArea();
    if (ImGui::Begin("Dump Video", nullptr)) {
        HandleGamepadBack(show);

        if (dumping) {
            ImGui::TextWrapped("Video dumping is active.");
            if (!active_path.empty()) {
                ImGui::TextWrapped("Output: %s", active_path.c_str());
            }
            ImGui::Spacing();
            if (ImGui::Button("Stop and finalize", ImVec2(220.0f, 0.0f))) {
                pending_stop = true;
                show = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Continue recording", ImVec2(220.0f, 0.0f))) {
                show = false;
            }
        } else {
            if (!status_message.empty()) {
                if (status_is_error) {
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                                       status_message.c_str());
                } else {
                    ImGui::TextDisabled("%s", status_message.c_str());
                }
            }

            if (!ffmpeg_available) {
                if (ImGui::Button("Retry FFmpeg load", ImVec2(190.0f, 0.0f))) {
                    ffmpeg_checked = false;
                    LoadFFmpegChoices();
                }
                ImGui::SameLine();
                if (ImGui::Button("Close", ImVec2(140.0f, 0.0f))) {
                    show = false;
                }
            } else {
                if (GetVirtualKeyboard().InputText("Output file", output_path.data(),
                                                   output_path.size())) {
                    path_is_default = false;
                }

                if (ChoiceCombo("Format", format_index, formats)) {
                    const auto& format = formats[static_cast<std::size_t>(format_index)];
                    video_encoder_index =
                        FindChoice(format.video_encoders, Settings::values.video_encoder,
                                   {"h264_mf", "mpeg4", "ffv1", "rawvideo"});
                    audio_encoder_index =
                        FindChoice(format.audio_encoders, Settings::values.audio_encoder,
                                   {"aac", "flac", "pcm_s16le"});
                    video_encoder_options.fill('\0');
                    audio_encoder_options.fill('\0');
                    if (path_is_default) {
                        SetDefaultOutputPath();
                    }
                }

                const auto& format = formats[static_cast<std::size_t>(format_index)];
                ChoiceCombo("Video encoder", video_encoder_index, format.video_encoders);
                ImGui::InputInt("Video bitrate", &video_bitrate, 100'000, 1'000'000);
                video_bitrate = std::max(0, video_bitrate);
                ChoiceCombo("Audio encoder", audio_encoder_index, format.audio_encoders);
                ImGui::InputInt("Audio bitrate", &audio_bitrate, 8'000, 32'000);
                audio_bitrate = std::max(0, audio_bitrate);

                if (ImGui::Button("Encoder options...", ImVec2(190.0f, 0.0f))) {
                    show_options = true;
                }

                ImGui::Separator();
                if (ImGui::Button("Start", ImVec2(140.0f, 0.0f))) {
                    std::filesystem::path path{output_path.data()};
                    if (path.empty()) {
                        status_message = "Please specify an output file.";
                        status_is_error = true;
                    } else {
                        if (path.is_relative()) {
                            path = std::filesystem::path{FileUtil::GetUserPath(
                                       FileUtil::UserPath::UserDir)} /
                                   "videos" / path;
                        }
                        if (!path.has_extension() && !format.extensions.empty()) {
                            path.replace_extension(format.extensions.front());
                        }
                        path = path.lexically_normal();
                        const std::string parent = path.parent_path().generic_string();
                        if (!parent.empty() && !FileUtil::IsDirectory(parent) &&
                            !FileUtil::CreateFullPath(parent + '/')) {
                            status_message = "Could not create output directory " + parent;
                            status_is_error = true;
                        } else {
                            ApplyConfiguration();
                            active_path = path.string();
                            pending_start = active_path;
                            status_message = "Starting video dumping...";
                            status_is_error = false;
                            show_options = false;
                            show = false;
                        }
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(140.0f, 0.0f))) {
                    show = false;
                }
            }
        }
    }
    ImGui::End();

    if (show_options) {
        SetNextWindowFullArea();
        if (ImGui::Begin("Encoder Options", nullptr)) {
            HandleGamepadBack(show_options);
            ImGui::TextWrapped(
                "Enter FFmpeg options as comma-separated key:value pairs. Unsupported options "
                "are reported when recording starts.");
            GetVirtualKeyboard().InputText("Format options", format_options.data(),
                                           format_options.size());
            GetVirtualKeyboard().InputText("Video encoder options", video_encoder_options.data(),
                                           video_encoder_options.size());
            GetVirtualKeyboard().InputText("Audio encoder options", audio_encoder_options.data(),
                                           audio_encoder_options.size());
            if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
                show_options = false;
            }
        }
        ImGui::End();
    }
}

void DumpingDialog::CompleteStart(bool success, std::string error) {
    dumping = success;
    if (success) {
        status_message = "Video dumping started.";
        status_is_error = false;
        show = false;
        return;
    }

    status_message = error.empty()
                         ? "Could not start video dumping. Check the encoder settings and log."
                         : std::move(error);
    status_is_error = true;
    show = true;
}

void DumpingDialog::CompleteStop(std::string error) {
    dumping = false;
    active_path.clear();
    if (error.empty()) {
        status_message = "Video dumping stopped and the output file was finalized.";
        status_is_error = false;
        show = false;
    } else {
        status_message = std::move(error);
        status_is_error = true;
        show = true;
    }
}

void CheatsDialog::Reload(Core::System& system) {
    cheats.clear();
    if (!system.IsPoweredOn()) {
        loaded = false;
        return;
    }

    const auto process = system.Kernel().GetCurrentProcess();
    if (!process) {
        loaded = false;
        return;
    }
    system.CheatEngine().LoadCheatFile(process->codeset->program_id);

    const auto& engine_cheats = system.CheatEngine().GetCheats();
    cheats.reserve(engine_cheats.size());
    for (const auto& cheat : engine_cheats) {
        CheatRow row;
        row.name = cheat->GetName();
        row.type = cheat->GetType();
        row.enabled = cheat->IsEnabled();
        cheats.push_back(std::move(row));
    }
    selected = -1;
    creating = false;
    validation_message.clear();
    SetTextBuffer(edit_name, {});
    SetTextBuffer(edit_code, {});
    SetTextBuffer(edit_comments, {});
    loaded = true;
}

void CheatsDialog::Draw(Core::System& system) {
    if (!show) {
        return;
    }

    SetNextWindowFullArea();
    if (ImGui::Begin("Cheats", nullptr)) {
        HandleGamepadBack(show);
        if (!system.IsPoweredOn()) {
            ImGui::TextWrapped("Cheats can only be edited while an application is running.");
            ImGui::End();
            return;
        }
        if (!loaded) {
            Reload(system);
        }

        const auto process = system.Kernel().GetCurrentProcess();
        if (!process) {
            ImGui::TextWrapped("The running application has no active process.");
            ImGui::End();
            return;
        }
        const u64 title_id = process->codeset->program_id;
        auto& engine = system.CheatEngine();

        const float list_width = ImGui::GetContentRegionAvail().x * 0.4f;
        if (ImGui::BeginChild("##cheat_list",
                              ImVec2(list_width, -ImGui::GetFrameHeightWithSpacing()),
                              ImGuiChildFlags_Borders)) {
            for (int i = 0; i < static_cast<int>(cheats.size()); ++i) {
                ImGui::PushID(i);
                bool enabled = cheats[i].enabled;
                if (ImGui::Checkbox("##enabled", &enabled)) {
                    cheats[i].enabled = enabled;
                    engine.GetCheats()[i]->SetEnabled(enabled);
                    engine.SaveCheatFile(title_id);
                }
                ImGui::SameLine();
                if (ImGui::Selectable(cheats[i].name.c_str(), selected == i)) {
                    selected = i;
                    creating = false;
                    validation_message.clear();
                    const auto& cheat = engine.GetCheats()[i];
                    SetTextBuffer(edit_name, cheat->GetName());
                    SetTextBuffer(edit_code, cheat->GetCode());
                    SetTextBuffer(edit_comments, cheat->GetComments());
                }
                ImGui::PopID();
            }
            if (creating) {
                ImGui::Selectable("[new cheat]", true);
            }
            if (cheats.empty()) {
                ImGui::TextDisabled("No cheats loaded for this title.");
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();
        if (ImGui::BeginChild("##cheat_editor", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()),
                              ImGuiChildFlags_Borders)) {
            ImGui::TextDisabled("Editor");
            ImGui::Separator();
            ImGui::BeginDisabled(selected < 0 && !creating);
            GetVirtualKeyboard().InputText("Name", edit_name.data(), edit_name.size());
            GetVirtualKeyboard().InputText("Comments", edit_comments.data(), edit_comments.size());
            ImGui::TextDisabled("Code");
            GetVirtualKeyboard().InputTextMultiline("##code", edit_code.data(), edit_code.size(),
                                                    ImVec2(-1.0f, -1.0f));
            ImGui::EndDisabled();
        }
        ImGui::EndChild();

        if (!validation_message.empty()) {
            ImGui::TextWrapped("%s", validation_message.c_str());
        }

        if (ImGui::Button("Reload", ImVec2(120.0f, 0.0f))) {
            loaded = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Add", ImVec2(120.0f, 0.0f))) {
            selected = -1;
            creating = true;
            validation_message.clear();
            SetTextBuffer(edit_name, {});
            SetTextBuffer(edit_code, {});
            SetTextBuffer(edit_comments, {});
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(selected < 0 && !creating);
        if (ImGui::Button("Save", ImVec2(120.0f, 0.0f))) {
            validation_message.clear();
            if (edit_name[0] == '\0') {
                validation_message = "Please enter a cheat name.";
            } else if (edit_code[0] == '\0') {
                validation_message = "Please enter at least one cheat-code line.";
            } else {
                std::istringstream code_stream{edit_code.data()};
                std::string line;
                std::size_t line_number = 0;
                while (std::getline(code_stream, line)) {
                    ++line_number;
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    if (!line.empty() && !Cheats::GatewayCheat::CheatLine{line}.valid) {
                        validation_message =
                            "Cheat-code line " + std::to_string(line_number) + " is invalid.";
                        break;
                    }
                }
            }

            if (validation_message.empty()) {
                auto cheat = std::make_shared<Cheats::GatewayCheat>(
                    edit_name.data(), edit_code.data(), edit_comments.data());
                if (creating) {
                    engine.AddCheat(std::move(cheat));
                    selected = static_cast<int>(engine.GetCheats().size()) - 1;
                    creating = false;
                } else if (selected >= 0 &&
                           selected < static_cast<int>(engine.GetCheats().size())) {
                    cheat->SetEnabled(engine.GetCheats()[selected]->IsEnabled());
                    engine.UpdateCheat(static_cast<std::size_t>(selected), std::move(cheat));
                }
                engine.SaveCheatFile(title_id);
                loaded = false;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(selected < 0 && !creating);
        if (ImGui::Button("Delete", ImVec2(120.0f, 0.0f))) {
            if (!creating && selected >= 0 &&
                selected < static_cast<int>(engine.GetCheats().size())) {
                engine.RemoveCheat(static_cast<std::size_t>(selected));
                engine.SaveCheatFile(title_id);
            }
            loaded = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
            show = false;
        }
    }
    ImGui::End();
}

void AboutDialog::Draw() {
    if (!show) {
        return;
    }

    SetNextWindowFullArea();
    if (ImGui::Begin("About Azahar", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        HandleGamepadBack(show);
        ImGui::TextUnformatted("Azahar");
        ImGui::TextDisabled("%s", Common::g_build_fullname);
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Azahar is an experimental open-source Nintendo 3DS emulator licensed under GPLv2.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "This build targets the Universal Windows Platform, using SDL2's WinRT backend and "
            "Mesa's D3D12 driver for OpenGL.");
        ImGui::Spacing();
        ImGui::TextDisabled(
            "This is an unofficial fork of Azahar. Please do not use the official Azahar GitHub "
            "or Discord servers to report issues with this fork; instead get help:");
        if (ImGui::SmallButton("Here##xbox_emulation_hub")) {
            pending_support_link = SupportLink::XboxEmulationHub;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Xbox Emulation Hub");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("or");
        ImGui::SameLine();
        if (ImGui::SmallButton("Here##revives_community")) {
            pending_support_link = SupportLink::RevivesCommunity;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Revives Community Server");
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled(
            "Nintendo 3DS is a trademark of Nintendo. Azahar is not affiliated with Nintendo.");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
            show = false;
        }
    }
    ImGui::End();
}

void LoadingScreen::Draw() {
    if (!visible) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("##loading", nullptr, flags)) {
        const ImVec2 size = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPosY(size.y * 0.42f);

        const auto centred = [&size](const char* text) {
            const float width = ImGui::CalcTextSize(text).x;
            ImGui::SetCursorPosX((size.x - width) * 0.5f);
            ImGui::TextUnformatted(text);
        };
        centred(game_title.empty() ? "Loading..." : game_title.c_str());
        ImGui::Spacing();
        centred(stage.c_str());
        ImGui::Spacing();

        constexpr float bar_width = 520.0f;
        ImGui::SetCursorPosX((size.x - bar_width) * 0.5f);
        // A negative fraction means "indeterminate"; ImGui animates the bar in that case.
        ImGui::ProgressBar(progress, ImVec2(bar_width, 0.0f));
    }
    ImGui::End();
}

} // namespace UWP
