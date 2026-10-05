// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace UWP {

/// The smaller Qt dialogs, one class per .ui file.

/// movie_record_dialog.ui and movie_play_dialog.ui.
class MovieDialog {
public:
    struct RecordRequest {
        std::string path;
        std::string author;
    };

    void Draw(Core::System& system);

    void OpenRecord() {
        show_record = true;
    }
    void OpenPlay() {
        show_play = true;
    }

    bool IsRecording() const {
        return recording;
    }
    bool IsPlaying() const {
        return playing;
    }
    bool IsReadOnly() const {
        return read_only;
    }
    bool IsDialogOpen() const {
        return show_record || show_play;
    }

    void SetMode(bool is_recording, bool is_playing) {
        recording = is_recording;
        playing = is_playing;
    }

    std::optional<RecordRequest> TakeRecordRequest() {
        return std::exchange(pending_record, std::nullopt);
    }
    std::optional<std::string> TakePlaybackRequest() {
        return std::exchange(pending_playback, std::nullopt);
    }

    void ToggleReadOnly(Core::System& system);
    void Close(Core::System& system);
    void SaveWithoutClosing(Core::System& system);

private:
    bool show_record = false;
    bool show_play = false;
    bool recording = false;
    bool playing = false;
    bool read_only = true;

    std::array<char, 512> record_path{};
    std::array<char, 128> record_author{};
    std::array<char, 512> play_path{};
    std::string validation_message;
    std::optional<RecordRequest> pending_record;
    std::optional<std::string> pending_playback;
};

/// dumping_dialog.ui plus its options_dialog.ui.
class DumpingDialog {
public:
    void Draw(Core::System& system);
    void Open();

    bool IsDumping() const {
        return dumping;
    }
    bool IsOpen() const {
        return show || show_options;
    }

    std::optional<std::string> TakeStartRequest() {
        return std::exchange(pending_start, std::nullopt);
    }
    bool TakeStopRequest() {
        return std::exchange(pending_stop, false);
    }

    void CompleteStart(bool success, std::string error = {});
    void CompleteStop(std::string error = {});

private:
    struct EncoderChoice {
        std::string name;
        std::string label;
    };

    struct FormatChoice {
        std::string name;
        std::string label;
        std::vector<std::string> extensions;
        std::vector<EncoderChoice> video_encoders;
        std::vector<EncoderChoice> audio_encoders;
    };

    void LoadFFmpegChoices();
    void SelectConfiguredValues();
    void ApplyConfiguration();
    void SetDefaultOutputPath();

    bool show = false;
    bool show_options = false;
    bool dumping = false;
    bool ffmpeg_checked = false;
    bool ffmpeg_available = false;
    bool path_is_default = true;
    bool pending_stop = false;
    bool status_is_error = false;

    std::array<char, 512> output_path{};
    std::array<char, 1024> format_options{};
    std::array<char, 1024> video_encoder_options{};
    std::array<char, 1024> audio_encoder_options{};
    std::vector<FormatChoice> formats;
    int format_index = 0;
    int video_encoder_index = 0;
    int audio_encoder_index = 0;
    int video_bitrate = 2500000;
    int audio_bitrate = 64000;
    std::string active_path;
    std::string status_message;
    std::optional<std::string> pending_start;
};

/// configure_cheats.ui and the cheat editor beside it.
class CheatsDialog {
public:
    void Draw(Core::System& system);

    void Open() {
        show = true;
        loaded = false;
    }
    bool IsOpen() const {
        return show;
    }

private:
    void Reload(Core::System& system);

    bool show = false;
    bool loaded = false;
    bool creating = false;
    int selected = -1;
    std::string validation_message;

    struct CheatRow {
        std::string name;
        std::string type;
        bool enabled = false;
    };
    std::vector<CheatRow> cheats;

    std::array<char, 128> edit_name{};
    std::array<char, 4096> edit_code{};
    std::array<char, 256> edit_comments{};
};

/// aboutdialog.ui.
class AboutDialog {
public:
    enum class SupportLink {
        None,
        XboxEmulationHub,
        RevivesCommunity,
    };

    void Draw();

    void Open() {
        show = true;
    }
    bool IsOpen() const {
        return show;
    }

    SupportLink TakeSupportLinkRequest() {
        return std::exchange(pending_support_link, SupportLink::None);
    }

private:
    bool show = false;
    SupportLink pending_support_link = SupportLink::None;
};

/// loading_screen.ui -- shown while a title is being prepared.
class LoadingScreen {
public:
    void Draw();

    void Show(std::string title) {
        game_title = std::move(title);
        visible = true;
    }
    void Hide() {
        visible = false;
    }
    void SetProgress(const char* stage_text, float fraction) {
        stage = stage_text;
        progress = fraction;
    }

    bool IsVisible() const {
        return visible;
    }

private:
    bool visible = false;
    std::string game_title;
    std::string stage = "Preparing...";
    float progress = 0.0f;
};

} // namespace UWP
