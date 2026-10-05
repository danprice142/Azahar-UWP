// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <unordered_set>

#include <fmt/format.h>
#include <glad/glad.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <json.hpp>
#include <lodepng.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <wrl/client.h>

#include "citra_uwp/ui/game_list.h"
#include "citra_uwp/ui/virtual_keyboard.h"
#include "citra_uwp/ui/widgets.h"
#include "citra_uwp/uwp_config.h"
#include "citra_uwp/uwp_gl.h"
#include "citra_uwp/uwp_settings.h"
#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/file_sys/archive_extsavedata.h"
#include "core/file_sys/archive_source_sd_savedata.h"
#include "core/file_sys/ncch_container.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/fs/archive.h"
#include "core/loader/loader.h"
#include "core/loader/smdh.h"

namespace UWP {

class VideoBackground {
public:
    ~VideoBackground() {
        SetPath({});
    }

    void SetPath(const std::string& path) {
        if (path == active_path) {
            return;
        }
        worker = {};
        active_path = path;
        {
            std::lock_guard lock{frame_mutex};
            frame.reset();
        }
        if (!path.empty()) {
            worker = std::jthread([this, path](std::stop_token stop) { Decode(path, stop); });
        }
    }

    std::shared_ptr<const GameListArtwork> CurrentFrame() const {
        std::lock_guard lock{frame_mutex};
        return frame;
    }

private:
    void Decode(const std::string& path, std::stop_token stop) {
        using Microsoft::WRL::ComPtr;
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        static std::once_flag startup_once;
        static HRESULT startup_result = E_FAIL;
        std::call_once(startup_once, [] { startup_result = MFStartup(MF_VERSION); });
        if (FAILED(startup_result)) {
            LOG_WARNING(Frontend, "Media Foundation startup failed: 0x{:08X}",
                        static_cast<u32>(startup_result));
            return;
        }

        ComPtr<IMFAttributes> reader_attributes;
        MFCreateAttributes(&reader_attributes, 1);
        reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        ComPtr<IMFSourceReader> reader;
        HRESULT result = MFCreateSourceReaderFromURL(Common::UTF8ToUTF16W(path).c_str(),
                                                     reader_attributes.Get(), &reader);
        if (FAILED(result)) {
            LOG_WARNING(Frontend, "Could not open ScreenScraper video {}: 0x{:08X}", path,
                        static_cast<u32>(result));
            return;
        }
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        ComPtr<IMFMediaType> requested_type;
        if (FAILED(MFCreateMediaType(&requested_type)) ||
            FAILED(requested_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(requested_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
            FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr,
                                               requested_type.Get()))) {
            LOG_WARNING(Frontend, "ScreenScraper video does not expose an RGB32 decode path: {}",
                        path);
            return;
        }
        ComPtr<IMFMediaType> media_type;
        UINT32 width = 0;
        UINT32 height = 0;
        if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &media_type)) ||
            FAILED(MFGetAttributeSize(media_type.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
            width == 0 || height == 0) {
            LOG_WARNING(Frontend, "Could not read ScreenScraper video dimensions: {}", path);
            return;
        }
        LOG_INFO(Frontend, "ScreenScraper video decoder ready for {} ({}x{})", path, width, height);

        constexpr u64 basis = 14695981039346656037ULL;
        constexpr u64 prime = 1099511628211ULL;
        u64 texture_key = basis;
        for (unsigned char byte : path) {
            texture_key = (texture_key ^ byte) * prime;
        }
        texture_key = texture_key == 0 ? 1 : texture_key;
        LONGLONG previous_timestamp = 0;
        u64 generation = 0;

        while (!stop.stop_requested()) {
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            result = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags,
                                        &timestamp, &sample);
            if (FAILED(result)) {
                break;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                PROPVARIANT position;
                PropVariantInit(&position);
                position.vt = VT_I8;
                position.hVal.QuadPart = 0;
                reader->SetCurrentPosition(GUID_NULL, position);
                previous_timestamp = 0;
                continue;
            }
            if (!sample) {
                continue;
            }
            if (previous_timestamp != 0 && timestamp > previous_timestamp) {
                const auto delay = std::chrono::microseconds{
                    std::min<LONGLONG>((timestamp - previous_timestamp) / 10, 100000)};
                std::this_thread::sleep_for(delay);
            }
            previous_timestamp = timestamp;

            ComPtr<IMFMediaBuffer> buffer;
            BYTE* source = nullptr;
            DWORD source_length = 0;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) ||
                FAILED(buffer->Lock(&source, nullptr, &source_length))) {
                continue;
            }
            const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
            if (source_length < row_bytes * height) {
                buffer->Unlock();
                continue;
            }
            auto next = std::make_shared<GameListArtwork>();
            next->width = width;
            next->height = height;
            next->texture_key = texture_key;
            next->generation = ++generation;
            next->rgba.resize(row_bytes * height);
            for (UINT32 y = 0; y < height; ++y) {
                const BYTE* source_row =
                    source + static_cast<std::size_t>(height - 1 - y) * row_bytes;
                u8* destination = next->rgba.data() + static_cast<std::size_t>(y) * row_bytes;
                for (UINT32 x = 0; x < width; ++x) {
                    destination[x * 4 + 0] = source_row[x * 4 + 2];
                    destination[x * 4 + 1] = source_row[x * 4 + 1];
                    destination[x * 4 + 2] = source_row[x * 4 + 0];
                    destination[x * 4 + 3] = 255;
                }
            }
            buffer->Unlock();
            {
                std::lock_guard lock{frame_mutex};
                frame = std::move(next);
            }
        }
    }

    std::string active_path;
    std::jthread worker;
    mutable std::mutex frame_mutex;
    std::shared_ptr<const GameListArtwork> frame;
};

namespace {

/// Extensions the Qt list scans for (game_list_worker.cpp).
constexpr std::array kSupportedExtensions{"3ds", "3dsx", "elf",  "axf",  "cci",  "cxi",
                                          "app", "cia",  "zcci", "zcxi", "z3dsx"};

constexpr float kFocusedRowScale = 1.20f;
constexpr float kDefaultArtworkScale = 1.42f;

float AdvanceRowFocusAnimation(std::unordered_map<int, float>& amounts, int index, bool focused) {
    auto position = amounts.try_emplace(index, 0.0f).first;
    float& amount = position->second;
    const float target = focused ? 1.0f : 0.0f;
    const float blend = 1.0f - std::exp(-14.0f * ImGui::GetIO().DeltaTime);
    amount += (target - amount) * blend;
    if (!focused && amount < 0.001f) {
        amounts.erase(position);
        return 1.0f;
    }
    const float eased = amount * amount * (3.0f - 2.0f * amount);
    return 1.0f + (kFocusedRowScale - 1.0f) * eased;
}

bool IsSupportedExtension(const std::string& path) {
    // GetExtensionFromFilename hands back a view into `path`, so materialise it before lowering.
    const std::string extension =
        Common::ToLower(std::string{FileUtil::GetExtensionFromFilename(path)});
    return std::find(kSupportedExtensions.begin(), kSupportedExtensions.end(), extension) !=
           kSupportedExtensions.end();
}

// The Segoe UI atlas only covers Latin; Japanese-only SMDH titles fall back to the filename.
bool IsTitleRenderable(const std::string& title) {
    if (title.empty()) {
        return false;
    }
    const std::wstring decoded = Common::UTF8ToUTF16W(title);
    return std::all_of(decoded.begin(), decoded.end(),
                       [](wchar_t character) { return character >= 0x20 && character <= 0x00FF; });
}

std::string CleanFilenameTitle(const std::string& filename) {
    std::string title =
        Common::UTF16ToUTF8(std::filesystem::path{Common::UTF8ToUTF16(filename)}.stem().wstring());
    const std::string lowered = Common::ToLower(title);
    if (const std::size_t decrypted = lowered.rfind("-decrypted");
        decrypted != std::string::npos && decrypted + 10 == lowered.size()) {
        title.erase(decrypted);
    }
    // Release tags are useful in filenames but not in a title label or metadata search.
    if (const std::size_t tags = title.find(" ("); tags != std::string::npos) {
        title.erase(tags);
    }
    while (!title.empty() && std::isspace(static_cast<unsigned char>(title.back()))) {
        title.pop_back();
    }
    return title.empty() ? filename : title;
}

const std::string& DisplayTitle(const GameListEntry& entry) {
    if (entry.scraped && !entry.scraped->name.empty()) {
        return entry.scraped->name;
    }
    return entry.title;
}

std::string RatingForEditor(std::string rating) {
    if (rating.empty()) {
        return rating;
    }
    char* end = nullptr;
    float score = std::strtof(rating.c_str(), &end);
    if (end == rating.c_str()) {
        return rating;
    }
    if (score > 5.0f) {
        score /= 4.0f;
    }
    return fmt::format("{:.1f}", std::clamp(score, 0.0f, 5.0f));
}

bool MetadataTextInput(const char* label, std::string& value, std::size_t capacity,
                       const char* hint = nullptr) {
    std::vector<char> buffer(std::max(capacity, value.size() + 1), '\0');
    std::copy(value.begin(), value.end(), buffer.begin());
    if (!GetVirtualKeyboard().InputText(label, buffer.data(), buffer.size(), hint)) {
        return false;
    }
    value = buffer.data();
    return true;
}

bool MetadataMultilineInput(const char* label, std::string& value, std::size_t capacity,
                            const ImVec2& size) {
    std::vector<char> buffer(std::max(capacity, value.size() + 1), '\0');
    std::copy(value.begin(), value.end(), buffer.begin());
    if (!GetVirtualKeyboard().InputTextMultiline(label, buffer.data(), buffer.size(), size)) {
        return false;
    }
    value = buffer.data();
    return true;
}

std::string FormatSize(u64 bytes) {
    constexpr std::array units{"B", "KiB", "MiB", "GiB"};
    auto value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    return fmt::format("{:.1f} {}", value, units[unit]);
}

std::string FormatDuration(u64 seconds) {
    if (seconds == 0) {
        return "Not played";
    }
    const double minutes = std::max(static_cast<double>(seconds) / 60.0, 1.0);
    if (minutes < 60.0) {
        return fmt::format("{:.0f} m", minutes);
    }
    return fmt::format("{:.1f} h", static_cast<double>(seconds) / 3600.0);
}

int RatingValue(const std::string& rating) {
    float score = 0.0f;
    if (!rating.empty()) {
        const char* begin = rating.c_str();
        char* end = nullptr;
        score = std::strtof(begin, &end);
        if (end == begin) {
            score = 0.0f;
        } else if (score > 5.0f) {
            score /= 4.0f; // ScreenScraper's community score is normally expressed out of 20.
        }
    }
    return std::clamp(static_cast<int>(std::lround(score)), 0, 5);
}

ImVec2 DrawStars(ImDrawList* draw_list, const ImVec2& cursor, int filled, float scale = 1.0f) {
    constexpr float pi = 3.14159265358979323846f;
    const float outer_radius = ImGui::GetFontSize() * 0.55f * scale;
    const float inner_radius = outer_radius * 0.43f;
    const float diameter = outer_radius * 2.0f;
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
    for (int star = 0; star < 5; ++star) {
        const ImVec2 centre{cursor.x + outer_radius + star * (diameter + gap),
                            cursor.y + outer_radius};
        ImVec2 points[10];
        for (int point = 0; point < 10; ++point) {
            const float radius = point % 2 == 0 ? outer_radius : inner_radius;
            const float angle = -pi * 0.5f + point * pi / 5.0f;
            points[point] =
                ImVec2(centre.x + std::cos(angle) * radius, centre.y + std::sin(angle) * radius);
        }
        const ImU32 color =
            star < filled ? IM_COL32(255, 194, 46, 255) : IM_COL32(105, 110, 122, 255);
        draw_list->AddConcavePolyFilled(points, 10, color);
    }
    return ImVec2(5.0f * diameter + 4.0f * gap, diameter);
}

void DrawFiveStarRating(const std::string& rating) {
    const ImVec2 size =
        DrawStars(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), RatingValue(rating));
    ImGui::Dummy(size);
}

bool MetadataEqual(const ScreenScraper::GameMetadata& lhs, const ScreenScraper::GameMetadata& rhs) {
    return lhs.program_id == rhs.program_id &&
           lhs.screenscraper_game_id == rhs.screenscraper_game_id && lhs.name == rhs.name &&
           lhs.publisher == rhs.publisher && lhs.developer == rhs.developer &&
           lhs.genre == rhs.genre && lhs.players == rhs.players &&
           lhs.release_date == rhs.release_date && lhs.rating == rhs.rating &&
           lhs.synopsis == rhs.synopsis && lhs.box_2d_path == rhs.box_2d_path &&
           lhs.box_3d_path == rhs.box_3d_path && lhs.box_2d_back_path == rhs.box_2d_back_path &&
           lhs.wheel_hd_path == rhs.wheel_hd_path && lhs.wheel_path == rhs.wheel_path &&
           lhs.support_2d_path == rhs.support_2d_path && lhs.mix_v1_path == rhs.mix_v1_path &&
           lhs.mix_v2_path == rhs.mix_v2_path && lhs.fanart_path == rhs.fanart_path &&
           lhs.screenshot_path == rhs.screenshot_path && lhs.video_path == rhs.video_path;
}

std::string MetadataRowPreview(const std::string& value) {
    if (value.empty()) {
        return "Not set";
    }
    std::string preview = value;
    std::replace_if(
        preview.begin(), preview.end(),
        [](unsigned char character) {
            return character == '\r' || character == '\n' || character == '\t';
        },
        ' ');
    for (std::size_t marker = preview.find("##"); marker != std::string::npos;
         marker = preview.find("##", marker + 3)) {
        preview.insert(marker + 1, " ");
    }
    constexpr std::size_t maximum_length = 96;
    if (preview.size() > maximum_length) {
        preview.resize(maximum_length);
        preview += "...";
    }
    return preview;
}

std::string GetRegion(const Loader::SMDH& smdh) {
    static constexpr std::array region_names{"Japan", "North America", "Europe", "Australia",
                                             "China", "Korea",         "Taiwan"};
    const auto regions = smdh.GetRegions();
    if (regions.empty()) {
        return "Invalid region";
    }
    if (regions.size() == region_names.size()) {
        return "Region free";
    }

    std::string result;
    for (const auto region : regions) {
        const auto index = static_cast<std::size_t>(region);
        if (index >= region_names.size()) {
            continue;
        }
        if (!result.empty()) {
            result += ", ";
        }
        result += region_names[index];
    }
    return result.empty() ? "Invalid region" : result;
}

std::string ScreenScraperRegion(std::string_view region) {
    if (region.find("Region free") != std::string_view::npos) {
        return "wor";
    }
    if (region.find("North America") != std::string_view::npos) {
        return "us";
    }
    if (region.find("Europe") != std::string_view::npos) {
        return "eu";
    }
    if (region.find("Japan") != std::string_view::npos) {
        return "jp";
    }
    if (region.find("Australia") != std::string_view::npos) {
        return "au";
    }
    if (region.find("China") != std::string_view::npos) {
        return "cn";
    }
    if (region.find("Korea") != std::string_view::npos) {
        return "kr";
    }
    if (region.find("Taiwan") != std::string_view::npos) {
        return "tw";
    }
    return {};
}

const char* ScreenScraperArtworkName(ScreenScraperBoxArt type) {
    switch (type) {
    case ScreenScraperBoxArt::Box2D:
        return "2D box";
    case ScreenScraperBoxArt::Box3D:
        return "3D box";
    case ScreenScraperBoxArt::MixRecalboxV1:
        return "Recalbox mix v1";
    case ScreenScraperBoxArt::MixRecalboxV2:
        return "Recalbox mix v2";
    case ScreenScraperBoxArt::FanArt:
        return "Fan art";
    case ScreenScraperBoxArt::Screenshot:
        return "Screenshot";
    case ScreenScraperBoxArt::GameLogo:
        return "Game logo";
    case ScreenScraperBoxArt::Box2DBack:
        return "2D box back";
    case ScreenScraperBoxArt::Support2D:
        return "2D cartridge";
    default:
        return "Default choices";
    }
}

bool IsApplication(u64 program_id) {
    const u32 high = static_cast<u32>(program_id >> 32);
    return high == 0x00040000 || high == 0x00040002 || high == 0x00040010;
}

bool IsUserApplication(u64 program_id) {
    const u32 high = static_cast<u32>(program_id >> 32);
    return high == 0x00040000 || high == 0x00040002;
}

u64 GetDlcTitleId(u64 program_id) {
    // Replace the category rather than OR-ing it, or a demo's category becomes a DLC one.
    return (program_id & 0xFFFFFFFFULL) | (0x0004008CULL << 32);
}

bool IsValidCartridgeImage(const std::string& path) {
    if (path.empty() || !FileUtil::Exists(path)) {
        return false;
    }
    FileSys::NCCHContainer cartridge{path};
    return cartridge.LoadHeader() == Loader::ResultStatus::Success && cartridge.IsNCSD();
}

bool IsSafeUninstallTarget(Service::FS::MediaType media_type, u64 title_id) {
    if (media_type != Service::FS::MediaType::SDMC || title_id == 0) {
        return false;
    }
    switch (static_cast<u32>(title_id >> 32)) {
    case 0x00040000:
    case 0x00040002:
    case 0x0004000E:
    case 0x0004008C:
        return FileUtil::Exists(Service::AM::GetTitlePath(media_type, title_id) + "content/");
    default:
        return false;
    }
}

const char* LoaderResultName(Loader::ResultStatus result) {
    switch (result) {
    case Loader::ResultStatus::Success:
        return "success";
    case Loader::ResultStatus::ErrorInvalidFormat:
        return "invalid format";
    case Loader::ResultStatus::ErrorNotImplemented:
        return "not supported by this file type";
    case Loader::ResultStatus::ErrorNotLoaded:
        return "content not loaded";
    case Loader::ResultStatus::ErrorNotUsed:
        return "content not present";
    case Loader::ResultStatus::ErrorEncrypted:
        return "content is encrypted";
    case Loader::ResultStatus::ErrorNotFound:
        return "content not found";
    default:
        return "loader error";
    }
}

std::vector<std::string> GetOpenGLShaderCachePaths(u64 program_id) {
    const std::string shader_dir = FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir);
    return {
        fmt::format("{}opengl/precompiled/separable/{:016X}.bin", shader_dir, program_id),
        fmt::format("{}opengl/precompiled/conventional/{:016X}.bin", shader_dir, program_id),
        fmt::format("{}opengl/transferable/{:016X}.bin", shader_dir, program_id),
    };
}

bool HasShaderCache(u64 program_id) {
    const auto paths = GetOpenGLShaderCachePaths(program_id);
    return std::any_of(paths.begin(), paths.end(), FileUtil::Exists);
}

const char* GetCompatibilityName(int compatibility) {
    switch (compatibility) {
    case 0:
        return "Perfect";
    case 1:
        return "Great";
    case 2:
        return "Okay";
    case 3:
        return "Bad";
    case 4:
        return "Intro/Menu";
    case 5:
        return "Won't Boot";
    case 99:
        return "Not Tested";
    default:
        return "Unknown";
    }
}

u64 HashSmdhMetadata(const std::vector<u8>& smdh_data) {
    // Hash the complete SMDH so an installed update with new metadata gets a fresh cache entry.
    constexpr u64 offset_basis = 14695981039346656037ULL;
    constexpr u64 prime = 1099511628211ULL;
    u64 hash = offset_basis;
    const std::size_t size = std::min(smdh_data.size(), sizeof(Loader::SMDH));
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= smdh_data[i];
        hash *= prime;
    }
    // Zero is kept as the "no icon" sentinel throughout the presentation code.
    return hash == 0 ? 1 : hash;
}

} // namespace

GameList::GameList()
    : presentation_thread{std::this_thread::get_id()},
      presentation_context{reinterpret_cast<std::uintptr_t>(wglGetCurrentContext())},
      top_menu_last_active{std::chrono::steady_clock::now()},
      video_background{std::make_unique<VideoBackground>()} {
    LoadCompatibilityList();
    const std::string inserted = UWP::values.inserted_cartridge.GetValue();
    if (!inserted.empty() && !IsValidCartridgeImage(inserted)) {
        LOG_WARNING(Frontend, "Clearing invalid inserted-cartridge path {}", inserted);
        UWP::values.inserted_cartridge.SetValue("");
        UWP::SaveConfig();
    }
}

GameList::~GameList() {
    StopPlayTimeTracking();
    CancelScan();
    if (management_future.valid()) {
        management_future.wait();
    }
    if (metadata_search_future.valid()) {
        metadata_search_future.wait();
    }
    if (metadata_scrape_future.valid()) {
        metadata_scrape_future.wait();
    }
    ReleaseIconTextures();
}

namespace {

/// Identifies an entry to remote frontends independently of list order and revision.
u64 RemoteToken(const std::string& path) {
    const u64 token =
        Common::ComputeHash64<Common::HashAlgo64::CityHash>(path.data(), path.size());
    return token != 0 ? token : 1;
}

} // Anonymous namespace

RemoteGameListSnapshot GameList::GetRemoteSnapshot() const {
    std::lock_guard lock{entries_mutex};
    RemoteGameListSnapshot snapshot;
    snapshot.revision = remote_revision.load(std::memory_order_relaxed);
    snapshot.entries.reserve(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        snapshot.entries.push_back({
            RemoteToken(entry.path),
            DisplayTitle(entry),
            entry.long_title,
            entry.file_type,
            entry.region,
            entry.program_id,
            entry.icon ? entry.icon->texture_key : 0,
            entry.media_type != Service::FS::MediaType::GameCard,
            entry.favorite,
            entry.path,
            entry.compatibility,
            entry.size_bytes,
            entry.play_time_seconds,
            entry.scraped,
        });
    }
    return snapshot;
}

RemoteLaunchResult GameList::ResolveRemoteLaunch(u32 revision, u64 token, std::string& path) const {
    std::lock_guard lock{entries_mutex};
    // Tokens outlive revisions, so a frontend holding an older catalogue can still launch.
    const auto entry = std::find_if(entries.begin(), entries.end(), [token](const auto& item) {
        return RemoteToken(item.path) == token;
    });
    if (entry == entries.end()) {
        return RemoteLaunchResult::UnknownEntry;
    }
    path = entry->path;
    return path.empty() ? RemoteLaunchResult::UnknownEntry : RemoteLaunchResult::Accepted;
}

std::optional<RemoteSelection> GameList::GetRemoteSelection() const {
    std::lock_guard lock{entries_mutex};
    if (selected_index < 0 || selected_index >= static_cast<int>(entries.size())) {
        return std::nullopt;
    }
    return RemoteSelection{remote_revision.load(std::memory_order_relaxed),
                           RemoteToken(entries[static_cast<std::size_t>(selected_index)].path)};
}

bool GameList::SelectRemote(u64 token) {
    std::lock_guard lock{entries_mutex};
    const auto entry = std::find_if(entries.begin(), entries.end(), [token](const auto& item) {
        return RemoteToken(item.path) == token;
    });
    if (entry == entries.end()) {
        return false;
    }
    selected_index = static_cast<int>(std::distance(entries.begin(), entry));
    remote_focus_index = selected_index;
    return true;
}

std::optional<u64> GameList::FindProgramIdByPath(const std::string& path) const {
    const auto normalized = [](std::string value) {
        std::replace(value.begin(), value.end(), '/', '\\');
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    };
    const std::string requested = normalized(path);
    std::lock_guard lock{entries_mutex};
    const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& candidate) {
        return normalized(candidate.path) == requested;
    });
    if (entry == entries.end() || entry->program_id == 0) {
        return std::nullopt;
    }
    return entry->program_id;
}

std::shared_ptr<const GameListIcon> GameList::GetRemoteIcon(u32 revision, u64 icon_key) const {
    std::lock_guard lock{entries_mutex};
    if (icon_key == 0) {
        return {};
    }
    for (const auto& entry : entries) {
        if (entry.icon && entry.icon->texture_key == icon_key) {
            return entry.icon;
        }
    }
    return {};
}

void GameList::LoadCompatibilityList() {
    const std::array candidates{
        FileUtil::GetExeDirectory() + DIR_SEP + "compatibility_list.json",
        FileUtil::GetExeDirectory() + DIR_SEP + "dist" + DIR_SEP + "compatibility_list" + DIR_SEP +
            "compatibility_list.json",
    };

    std::string contents;
    std::string loaded_path;
    for (const auto& candidate : candidates) {
        contents.clear();
        if (FileUtil::ReadFileToString(true, candidate, contents) && !contents.empty()) {
            loaded_path = candidate;
            break;
        }
    }
    if (loaded_path.empty()) {
        LOG_WARNING(Frontend, "Compatibility list is not present next to the UWP executable");
        return;
    }

    const auto document = nlohmann::json::parse(contents, nullptr, false);
    if (document.is_discarded() || !document.is_array()) {
        LOG_ERROR(Frontend, "Compatibility list {} is invalid", loaded_path);
        return;
    }

    for (const auto& game : document) {
        if (!game.is_object()) {
            continue;
        }
        const auto compatibility = game.find("compatibility");
        const auto releases = game.find("releases");
        if (compatibility == game.end() || !compatibility->is_number_integer() ||
            releases == game.end() || !releases->is_array()) {
            continue;
        }
        const std::string status = GetCompatibilityName(compatibility->get<int>());
        for (const auto& release : *releases) {
            if (!release.is_object()) {
                continue;
            }
            const auto id = release.find("id");
            if (id == release.end() || !id->is_string()) {
                continue;
            }
            const std::string text = id->get<std::string>();
            u64 program_id{};
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), program_id, 16);
            if (error == std::errc{} && end == text.data() + text.size()) {
                compatibility_list[program_id] = status;
            }
        }
    }
    LOG_INFO(Frontend, "Loaded {} UWP compatibility entries from {}", compatibility_list.size(),
             loaded_path);
}

std::shared_ptr<const GameListIcon> GameList::GetCachedIcon(const std::vector<u8>& smdh_data,
                                                            bool large) {
    if (!Loader::IsValidSMDH(smdh_data)) {
        return {};
    }

    // Small mode uses the native 24x24 icon rather than downscaling the 48x48 one.
    constexpr u64 prime = 1099511628211ULL;
    u64 texture_key = HashSmdhMetadata(smdh_data);
    texture_key ^= large ? 48 : 24;
    texture_key *= prime;
    if (texture_key == 0) {
        texture_key = 1;
    }
    {
        std::lock_guard lock{icon_cache_mutex};
        if (const auto cached = decoded_icon_cache.find(texture_key);
            cached != decoded_icon_cache.end()) {
            if (auto icon = cached->second.lock()) {
                return icon;
            }
        }
    }

    Loader::SMDH smdh;
    std::memcpy(&smdh, smdh_data.data(), sizeof(smdh));

    // SMDH icons are RGB565 in 8x8 Morton tiles; the core helper detiles them.
    const u32 dimension = large ? 48 : 24;
    std::vector<u16> decoded = smdh.GetIcon(large);
    if (decoded.size() != dimension * dimension) {
        LOG_WARNING(Frontend, "SMDH icon decoder returned {} pixels instead of {}", decoded.size(),
                    dimension * dimension);
        return {};
    }

    auto icon = std::make_shared<GameListIcon>();
    icon->rgb565 = std::move(decoded);
    icon->width = dimension;
    icon->height = dimension;
    icon->texture_key = texture_key;
    {
        std::lock_guard lock{icon_cache_mutex};
        decoded_icon_cache[texture_key] = icon;
    }
    return icon;
}

bool GameList::IsPresentationContextCurrent() const {
    return std::this_thread::get_id() == presentation_thread && presentation_context != 0 &&
           reinterpret_cast<std::uintptr_t>(wglGetCurrentContext()) == presentation_context;
}

unsigned int GameList::GetOrCreateIconTexture(const GameListEntry& entry) {
    if (!entry.icon) {
        return 0;
    }

    const u64 key = entry.icon->texture_key;
    if (const auto cached = icon_textures.find(key); cached != icon_textures.end()) {
        return cached->second;
    }
    if (!IsPresentationContextCurrent()) {
        if (!context_warning_logged) {
            LOG_ERROR(Frontend,
                      "Game-list icon upload skipped because the presentation context is not "
                      "current on its owning thread");
            context_warning_logged = true;
        }
        return 0;
    }

    // ImGui's font upload can leave a non-default unpack alignment/binding behind.
    GLint previous_active_texture = GL_TEXTURE0;
    GLint previous_texture = 0;
    GLint previous_unpack_alignment = 4;
    GLint previous_unpack_row_length = 0;
    GLint previous_unpack_skip_rows = 0;
    GLint previous_unpack_skip_pixels = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_unpack_alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &previous_unpack_row_length);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &previous_unpack_skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &previous_unpack_skip_pixels);

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, static_cast<GLsizei>(entry.icon->width),
                 static_cast<GLsizei>(entry.icon->height), 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5,
                 entry.icon->rgb565.data());

    glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, previous_unpack_row_length);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, previous_unpack_skip_rows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, previous_unpack_skip_pixels);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
    glActiveTexture(static_cast<GLenum>(previous_active_texture));

    if (texture != 0) {
        icon_textures.emplace(key, texture);
    }
    return texture;
}

unsigned int GameList::GetOrCreateArtworkTexture(
    const std::shared_ptr<const GameListArtwork>& artwork) {
    if (!artwork || artwork->rgba.empty()) {
        return 0;
    }
    if (const auto cached = artwork_textures.find(artwork->texture_key);
        cached != artwork_textures.end()) {
        if (artwork->generation != 0 &&
            artwork_uploaded_generation[artwork->texture_key] != artwork->generation) {
            GLint previous_texture = 0;
            GLint previous_unpack_alignment = 4;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_unpack_alignment);
            glBindTexture(GL_TEXTURE_2D, cached->second);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(artwork->width),
                         static_cast<GLsizei>(artwork->height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         artwork->rgba.data());
            glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
            artwork_uploaded_generation[artwork->texture_key] = artwork->generation;
        }
        return cached->second;
    }
    if (!IsPresentationContextCurrent()) {
        return 0;
    }
    GLint previous_texture = 0;
    GLint previous_unpack_alignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_unpack_alignment);
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(artwork->width),
                 static_cast<GLsizei>(artwork->height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 artwork->rgba.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
    if (texture != 0) {
        artwork_textures.emplace(artwork->texture_key, texture);
        artwork_uploaded_generation[artwork->texture_key] = artwork->generation;
    }
    return texture;
}

void GameList::PruneIconTextures(bool icons_enabled) {
    if (!IsPresentationContextCurrent()) {
        return;
    }

    std::unordered_set<u64> live_keys;
    if (icons_enabled) {
        live_keys.reserve(entries.size() + (details_entry.has_value() ? 1 : 0));
        for (const auto& entry : entries) {
            if (entry.icon) {
                live_keys.insert(entry.icon->texture_key);
            }
        }
        if (details_entry && details_entry->icon) {
            live_keys.insert(details_entry->icon->texture_key);
        }
    }

    for (auto texture = icon_textures.begin(); texture != icon_textures.end();) {
        if (live_keys.contains(texture->first)) {
            ++texture;
            continue;
        }
        const GLuint handle = texture->second;
        glDeleteTextures(1, &handle);
        texture = icon_textures.erase(texture);
    }

    std::lock_guard cache_lock{icon_cache_mutex};
    for (auto icon = decoded_icon_cache.begin(); icon != decoded_icon_cache.end();) {
        if (icon->second.expired()) {
            icon = decoded_icon_cache.erase(icon);
        } else {
            ++icon;
        }
    }

    std::unordered_set<u64> live_artwork;
    for (const auto& entry : entries) {
        if (entry.box_art) {
            live_artwork.insert(entry.box_art->texture_key);
        }
        if (entry.large_box_art) {
            live_artwork.insert(entry.large_box_art->texture_key);
        }
        if (entry.background) {
            live_artwork.insert(entry.background->texture_key);
        }
    }
    for (auto artwork = artwork_textures.begin(); artwork != artwork_textures.end();) {
        if (live_artwork.contains(artwork->first)) {
            ++artwork;
        } else {
            const GLuint handle = artwork->second;
            glDeleteTextures(1, &handle);
            artwork_uploaded_generation.erase(artwork->first);
            artwork = artwork_textures.erase(artwork);
        }
    }
}

void GameList::ReleaseIconTextures() {
    if (icon_textures.empty() && artwork_textures.empty()) {
        return;
    }
    if (!IsPresentationContextCurrent()) {
        LOG_WARNING(Frontend,
                    "Leaving {} game-list icon textures for context teardown because the "
                    "presentation context is no longer current",
                    icon_textures.size() + artwork_textures.size());
        icon_textures.clear();
        artwork_textures.clear();
        artwork_uploaded_generation.clear();
        return;
    }

    std::vector<GLuint> handles;
    handles.reserve(icon_textures.size());
    for (const auto& [key, texture] : icon_textures) {
        (void)key;
        handles.push_back(texture);
    }
    for (const auto& [key, texture] : artwork_textures) {
        (void)key;
        handles.push_back(texture);
    }
    glDeleteTextures(static_cast<GLsizei>(handles.size()), handles.data());
    icon_textures.clear();
    artwork_textures.clear();
    artwork_uploaded_generation.clear();
}

void GameList::CancelScan() {
    stop_scanning = true;
    if (scan_thread.joinable()) {
        scan_thread.join();
    }
    if (scrape_thread.joinable()) {
        scrape_thread.join();
    }
    scanning = false;
    scraping_metadata = false;
}

std::optional<std::string> GameList::FindPathByProgramId(u64 program_id) const {
    std::scoped_lock lock{entries_mutex};
    const auto entry = std::find_if(entries.begin(), entries.end(), [program_id](const auto& item) {
        return item.program_id == program_id;
    });
    if (entry == entries.end()) {
        return std::nullopt;
    }
    return entry->path;
}

void GameList::Refresh() {
    if (scanning || scraping_metadata) {
        return;
    }
    if (scan_thread.joinable()) {
        scan_thread.join();
    }
    if (scrape_thread.joinable()) {
        scrape_thread.join();
    }

    const std::string directory = UWP::values.game_directory.GetValue();
    if (directory.empty() || !FileUtil::IsDirectory(directory)) {
        return;
    }
    LOG_INFO(Frontend, "Scanning for applications in {}", directory);

    scanning = true;
    focus_first_game = true;
    stop_scanning = false;
    const bool deep_scan = UWP::values.game_directory_deep_scan.GetValue();
    const bool hide_without_icon = UWP::values.game_list_hide_no_icon.GetValue();
    const int icon_size = std::clamp(UWP::values.game_list_icon_size.GetValue(), 0, 2);
    const auto favorite_ids = UWP::values.favorited_ids;
    const bool favorite_first = UWP::values.game_list_favorite_first.GetValue();
    scan_thread = std::thread([this, directory, deep_scan, hide_without_icon, icon_size,
                               favorite_ids, favorite_first]() mutable {
        try {
            ScanDirectories({directory}, deep_scan, hide_without_icon, icon_size,
                            std::move(favorite_ids), favorite_first);
        } catch (const std::exception& exception) {
            LOG_ERROR(Frontend, "Game library worker failed: {}", exception.what());
            scanning = false;
            scraping_metadata = false;
        } catch (...) {
            LOG_ERROR(Frontend, "Game library worker failed with an unknown exception");
            scanning = false;
            scraping_metadata = false;
        }
    });
}

void GameList::ScanDirectories(std::vector<std::string> directories, bool deep_scan,
                               bool hide_without_icon, int icon_size, std::vector<u64> favorite_ids,
                               bool favorite_first) {
    std::vector<GameListEntry> found;

    // ForeachDirectoryEntry is single-level; the depth cap contains directory cycles.
    constexpr int kMaxDepth = 256;
    std::function<void(const std::string&, int, Service::FS::MediaType)> scan =
        [&](const std::string& dir, int remaining_depth, Service::FS::MediaType media_type) {
            const auto visit = [&](u64* /*num_entries*/, const std::string& parent,
                                   const std::string& name) -> bool {
                if (stop_scanning) {
                    return false;
                }
                const std::string path = parent + DIR_SEP + name;
                if (FileUtil::IsDirectory(path)) {
                    if (remaining_depth > 0) {
                        scan(path, remaining_depth - 1, media_type);
                    }
                    return true;
                }
                if (!IsSupportedExtension(path)) {
                    return true;
                }

                GameListEntry entry;
                entry.path = path;
                entry.size_bytes = FileUtil::GetSize(path);
                entry.media_type = media_type;

                const auto loader = Loader::GetLoader(path);
                if (loader != nullptr) {
                    bool executable = false;
                    const Loader::ResultStatus executable_status = loader->IsExecutable(executable);
                    if (!executable && executable_status != Loader::ResultStatus::ErrorEncrypted) {
                        return true;
                    }
                    entry.encrypted = executable_status == Loader::ResultStatus::ErrorEncrypted;

                    loader->ReadProgramId(entry.program_id);
                    loader->ReadExtdataId(entry.extdata_id);
                    entry.can_insert = loader->GetFileType() == Loader::FileType::CCI &&
                                       IsValidCartridgeImage(path);
                    entry.favorite =
                        entry.program_id != 0 && std::find(favorite_ids.begin(), favorite_ids.end(),
                                                           entry.program_id) != favorite_ids.end();
                    entry.file_type = Loader::GetFileTypeString(loader->GetFileType(),
                                                                loader->IsFileCompressed());

                    std::vector<u8> smdh_data;
                    // Prefer the update's SMDH, as the Qt worker does, falling back to the
                    // original.
                    if (!(entry.program_id & ~0x00040000FFFFFFFFULL)) {
                        const std::string update_path = Service::AM::GetTitleContentPath(
                            Service::FS::MediaType::SDMC,
                            Service::AM::GetTitleUpdateId(entry.program_id));
                        if (FileUtil::Exists(update_path)) {
                            if (const auto update_loader = Loader::GetLoader(update_path)) {
                                update_loader->ReadIcon(smdh_data);
                            }
                        }
                    }
                    if (!Loader::IsValidSMDH(smdh_data)) {
                        loader->ReadIcon(smdh_data);
                    }

                    const bool has_icon = Loader::IsValidSMDH(smdh_data);
                    entry.has_icon = has_icon;
                    const bool is_system_title =
                        ((entry.program_id >> 32) & 0xFFFFFFFFULL) == 0x00040010ULL;
                    if (is_system_title) {
                        if (!has_icon || !reinterpret_cast<const Loader::SMDH*>(smdh_data.data())
                                              ->flags.visible) {
                            return true;
                        }
                    } else if (hide_without_icon && !has_icon) {
                        return true;
                    }

                    if (has_icon) {
                        Loader::SMDH smdh;
                        std::memcpy(&smdh, smdh_data.data(), sizeof(smdh));
                        if (icon_size != 0) {
                            entry.icon = GetCachedIcon(smdh_data, icon_size == 2);
                        }
                        entry.title = Common::UTF16BufferToUTF8(
                            smdh.GetShortTitle(Loader::SMDH::TitleLanguage::English));
                        entry.long_title = Common::UTF16BufferToUTF8(
                            smdh.GetLongTitle(Loader::SMDH::TitleLanguage::English));
                        entry.region = GetRegion(smdh);
                    } else {
                        entry.region = "Invalid region";
                    }

                    if (entry.title.empty()) {
                        loader->ReadTitle(entry.title);
                    }
                    if (entry.long_title.empty()) {
                        entry.long_title = entry.title;
                    }
                    if (const auto compatibility = compatibility_list.find(entry.program_id);
                        compatibility != compatibility_list.end()) {
                        entry.compatibility = compatibility->second;
                    }
                    entry.play_time_seconds = play_time_manager.GetPlayTime(entry.program_id);
                } else {
                    return true;
                }
                if (entry.title.empty()) {
                    entry.title = name;
                }
                if (!IsTitleRenderable(entry.title)) {
                    entry.title = CleanFilenameTitle(name);
                }
                if (entry.long_title.empty()) {
                    entry.long_title = entry.title;
                } else if (!IsTitleRenderable(entry.long_title)) {
                    entry.long_title = entry.title;
                }

                found.push_back(std::move(entry));
                return true;
            };

            if (FileUtil::IsDirectory(dir)) {
                FileUtil::ForeachDirectoryEntry(nullptr, dir, visit);
            }
        };

    for (const std::string& root : directories) {
        if (stop_scanning) {
            break;
        }
        scan(root, deep_scan ? kMaxDepth : 0, Service::FS::MediaType::GameCard);
    }

    // Installed and system titles are scanned into the same list as the user library.
    const std::string sdmc = FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir);
    const std::string nand = FileUtil::GetUserPath(FileUtil::UserPath::NANDDir);
    const std::string id_path = "Nintendo 3DS/00000000000000000000000000000000/"
                                "00000000000000000000000000000000/title/";
    if (!stop_scanning) {
        scan(sdmc + id_path + "00040000" + DIR_SEP, 2, Service::FS::MediaType::SDMC);
        scan(sdmc + id_path + "00040002" + DIR_SEP, 2, Service::FS::MediaType::SDMC);
        scan(nand + "00000000000000000000000000000000/title/00040010" + DIR_SEP, 2,
             Service::FS::MediaType::NAND);
    }

    std::sort(found.begin(), found.end(), [favorite_first](const auto& lhs, const auto& rhs) {
        if (favorite_first && lhs.favorite != rhs.favorite) {
            return lhs.favorite;
        }
        const std::string lhs_title = Common::ToLower(lhs.title);
        const std::string rhs_title = Common::ToLower(rhs.title);
        if (lhs_title != rhs_title) {
            return lhs_title < rhs_title;
        }
        const std::string lhs_path = Common::ToLower(lhs.path);
        const std::string rhs_path = Common::ToLower(rhs.path);
        if (lhs_path != rhs_path) {
            return lhs_path < rhs_path;
        }
        // Keep the dedicated SD/NAND record so management never mistakes it for a loose file.
        return static_cast<u32>(lhs.media_type) < static_cast<u32>(rhs.media_type);
    });
    found.erase(std::unique(found.begin(), found.end(),
                            [](const auto& lhs, const auto& rhs) {
                                return Common::ToLower(lhs.path) == Common::ToLower(rhs.path);
                            }),
                found.end());

    if (UWP::values.screenscraper_enabled.GetValue()) {
        for (auto& entry : found) {
            ApplyCachedMetadata(entry);
        }
    }

    if (!stop_scanning) {
        std::lock_guard lock{entries_mutex};
        entries = std::move(found);
        remote_revision.fetch_add(1, std::memory_order_relaxed);
        icon_prune_requested = true;
    }
    LOG_INFO(Frontend, "Game list scan finished");
    const bool auto_scrape = !stop_scanning && UWP::values.screenscraper_enabled.GetValue() &&
                             UWP::values.screenscraper_auto_scrape.GetValue() &&
                             ScreenScraper::HasDeveloperCredentials() &&
                             ScreenScraper::HasUserCredentials();
    if (auto_scrape) {
        // Continue on this worker; reassigning scrape_thread here could replace a joinable
        // thread.
        scraping_metadata = true;
    }
    scanning = false;
    if (auto_scrape) {
        ScrapeMissingMetadata(false);
    }
}

std::shared_ptr<const GameListArtwork> GameList::LoadArtwork(const std::string& path) {
    if (path.empty() || !FileUtil::Exists(path)) {
        return {};
    }
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(result)) {
        result = factory->CreateDecoderFromFilename(Common::UTF8ToUTF16W(path).c_str(), nullptr,
                                                    GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                    &decoder);
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(result)) {
        result = decoder->GetFrame(0, &frame);
    }
    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(result)) {
        result = frame->GetSize(&width, &height);
    }
    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(result)) {
        result = factory->CreateFormatConverter(&converter);
    }
    if (SUCCEEDED(result)) {
        result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom);
    }
    const u64 byte_count = static_cast<u64>(width) * static_cast<u64>(height) * 4;
    if (FAILED(result) || width == 0 || height == 0 || byte_count > UINT_MAX) {
        LOG_WARNING(Frontend, "Could not decode ScreenScraper artwork {}: 0x{:08X}", path,
                    static_cast<u32>(result));
        return {};
    }
    auto artwork = std::make_shared<GameListArtwork>();
    artwork->rgba.resize(static_cast<std::size_t>(byte_count));
    result = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(byte_count),
                                   artwork->rgba.data());
    if (FAILED(result)) {
        LOG_WARNING(Frontend, "Could not read ScreenScraper artwork pixels {}: 0x{:08X}", path,
                    static_cast<u32>(result));
        return {};
    }
    artwork->width = width;
    artwork->height = height;
    constexpr u64 basis = 14695981039346656037ULL;
    constexpr u64 prime = 1099511628211ULL;
    u64 key = basis;
    for (unsigned char byte : path) {
        key = (key ^ byte) * prime;
    }
    artwork->texture_key = key == 0 ? 1 : key;
    return artwork;
}

std::shared_ptr<const GameListArtwork> GameList::LoadArtwork(const std::vector<u8>& encoded,
                                                             u64 texture_key) {
    if (encoded.empty() || encoded.size() > UINT_MAX) {
        return {};
    }
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
    ComPtr<IWICStream> stream;
    if (SUCCEEDED(result)) {
        result = factory->CreateStream(&stream);
    }
    if (SUCCEEDED(result)) {
        result = stream->InitializeFromMemory(const_cast<BYTE*>(encoded.data()),
                                              static_cast<DWORD>(encoded.size()));
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(result)) {
        result = factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                  WICDecodeMetadataCacheOnLoad, &decoder);
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(result)) {
        result = decoder->GetFrame(0, &frame);
    }
    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(result)) {
        result = frame->GetSize(&width, &height);
    }
    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(result)) {
        result = factory->CreateFormatConverter(&converter);
    }
    if (SUCCEEDED(result)) {
        result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom);
    }
    const u64 byte_count = static_cast<u64>(width) * static_cast<u64>(height) * 4;
    if (FAILED(result) || width == 0 || height == 0 || byte_count > UINT_MAX) {
        LOG_WARNING(Frontend, "Could not decode ScreenScraper search preview: 0x{:08X}",
                    static_cast<u32>(result));
        return {};
    }
    auto artwork = std::make_shared<GameListArtwork>();
    artwork->rgba.resize(static_cast<std::size_t>(byte_count));
    result = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(byte_count),
                                   artwork->rgba.data());
    if (FAILED(result)) {
        LOG_WARNING(Frontend, "Could not read ScreenScraper search preview pixels: 0x{:08X}",
                    static_cast<u32>(result));
        return {};
    }
    artwork->width = width;
    artwork->height = height;
    artwork->texture_key = texture_key == 0 ? 1 : texture_key;
    return artwork;
}

void GameList::ApplyCachedMetadata(GameListEntry& entry) {
    entry.scraped = ScreenScraper::LoadCached(entry.program_id);
    if (!entry.scraped) {
        return;
    }
    entry.box_art.reset();
    const std::string box_path = ScreenScraper::SelectedBoxArtPath(*entry.scraped);
    if (!box_path.empty()) {
        entry.box_art = LoadArtwork(box_path);
    }
    entry.large_box_art.reset();
    const std::string large_box_path = ScreenScraper::SelectedLargeBoxArtPath(*entry.scraped);
    if (!large_box_path.empty()) {
        entry.large_box_art = LoadArtwork(large_box_path);
    }
    entry.background.reset();
    const auto background_type = UWP::values.screenscraper_background.GetValue();
    if (background_type != ScreenScraperBackground::Off) {
        const std::string background_path = ScreenScraper::SelectedBackgroundPath(*entry.scraped);
        if (background_type != ScreenScraperBackground::Video ||
            background_path != entry.scraped->video_path) {
            entry.background = LoadArtwork(background_path);
        }
    }
}

void GameList::ScrapeMissingMetadata(bool force_all) {
    std::vector<ScreenScraper::ScrapeRequest> requests;
    try {
        {
            std::lock_guard lock{entries_mutex};
            for (const auto& entry : entries) {
                const bool missing_enabled_media =
                    entry.scraped &&
                    ((UWP::values.screenscraper_download_box_2d.GetValue() &&
                      entry.scraped->box_2d_path.empty()) ||
                     (UWP::values.screenscraper_download_box_3d.GetValue() &&
                      entry.scraped->box_3d_path.empty()) ||
                     (UWP::values.screenscraper_download_box_2d_back.GetValue() &&
                      entry.scraped->box_2d_back_path.empty()) ||
                     (UWP::values.screenscraper_download_game_logo.GetValue() &&
                      entry.scraped->wheel_hd_path.empty() && entry.scraped->wheel_path.empty()) ||
                     (UWP::values.screenscraper_download_support_2d.GetValue() &&
                      entry.scraped->support_2d_path.empty()) ||
                     (UWP::values.screenscraper_download_mix_v1.GetValue() &&
                      entry.scraped->mix_v1_path.empty()) ||
                     (UWP::values.screenscraper_download_mix_v2.GetValue() &&
                      entry.scraped->mix_v2_path.empty()) ||
                     (UWP::values.screenscraper_download_fanart.GetValue() &&
                      entry.scraped->fanart_path.empty()) ||
                     (UWP::values.screenscraper_download_screenshot.GetValue() &&
                      entry.scraped->screenshot_path.empty()) ||
                     (UWP::values.screenscraper_download_video.GetValue() &&
                      entry.scraped->video_path.empty()));
                if ((force_all || !entry.scraped || missing_enabled_media) &&
                    entry.program_id != 0) {
                    ScreenScraper::ScrapeRequest request;
                    request.program_id = entry.program_id;
                    request.title = entry.title;
                    request.path = entry.path;
                    request.size_bytes = entry.size_bytes;
                    request.preferred_region = ScreenScraperRegion(entry.region);
                    requests.push_back(std::move(request));
                }
            }
        }
        for (const auto& request : requests) {
            if (stop_scanning) {
                break;
            }
            try {
                auto result = ScreenScraper::Scrape(request, &stop_scanning);
                if (!result.success || !result.metadata) {
                    LOG_WARNING(Frontend, "ScreenScraper failed for {}: {}", request.title,
                                result.message);
                    continue;
                }
                LOG_INFO(Frontend, "{}", result.message);
                std::lock_guard lock{entries_mutex};
                const auto target =
                    std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
                        return entry.program_id == request.program_id;
                    });
                if (target != entries.end()) {
                    ApplyCachedMetadata(*target);
                    icon_prune_requested = true;
                    remote_revision.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (const std::exception& exception) {
                LOG_ERROR(Frontend, "ScreenScraper failed for {}: {}", request.title,
                          exception.what());
            } catch (...) {
                LOG_ERROR(Frontend, "ScreenScraper failed for {} with an unknown exception",
                          request.title);
            }
        }
    } catch (const std::exception& exception) {
        LOG_ERROR(Frontend, "ScreenScraper library operation failed: {}", exception.what());
    } catch (...) {
        LOG_ERROR(Frontend, "ScreenScraper library operation failed with an unknown exception");
    }
    scraping_metadata = false;
}

void GameList::ScrapeLibrary() {
    if (scanning || scraping_metadata || !UWP::values.screenscraper_enabled.GetValue() ||
        !ScreenScraper::HasDeveloperCredentials() || !ScreenScraper::HasUserCredentials()) {
        return;
    }
    if (scrape_thread.joinable()) {
        scrape_thread.join();
    }
    stop_scanning = false;
    scraping_metadata = true;
    scrape_thread = std::thread(&GameList::ScrapeMissingMetadata, this, true);
}

void GameList::ApplyLibraryPreferences() {
    selected_index = -1;
    focus_first_game = true;
    SortEntries();
}

bool GameList::MatchesFilter(const GameListEntry& entry) const {
    if (UWP::values.game_list_hide_no_icon.GetValue() && !entry.has_icon) {
        return false;
    }
    if (UWP::values.game_list_favorites_only.GetValue() && !entry.favorite) {
        return false;
    }
    if (filter_text.empty()) {
        return true;
    }
    const std::string needle = Common::ToLower(filter_text);
    return Common::ToLower(entry.title).find(needle) != std::string::npos ||
           (entry.scraped &&
            Common::ToLower(entry.scraped->name).find(needle) != std::string::npos) ||
           Common::ToLower(entry.long_title).find(needle) != std::string::npos ||
           Common::ToLower(entry.path).find(needle) != std::string::npos ||
           Common::ToLower(entry.region).find(needle) != std::string::npos ||
           Common::ToLower(entry.compatibility).find(needle) != std::string::npos ||
           Common::ToLower(fmt::format("{:016X}", entry.program_id)).find(needle) !=
               std::string::npos;
}

void GameList::SortEntries() {
    std::lock_guard lock{entries_mutex};
    std::string selected_path;
    if (selected_index >= 0 && selected_index < static_cast<int>(entries.size())) {
        selected_path = entries[static_cast<std::size_t>(selected_index)].path;
    }

    const bool favorite_first = UWP::values.game_list_favorite_first.GetValue();
    std::sort(entries.begin(), entries.end(), [favorite_first](const auto& lhs, const auto& rhs) {
        if (favorite_first && lhs.favorite != rhs.favorite) {
            return lhs.favorite;
        }
        const std::string lhs_title = Common::ToLower(DisplayTitle(lhs));
        const std::string rhs_title = Common::ToLower(DisplayTitle(rhs));
        if (lhs_title != rhs_title) {
            return lhs_title < rhs_title;
        }
        return Common::ToLower(lhs.path) < Common::ToLower(rhs.path);
    });

    selected_index = -1;
    if (!selected_path.empty()) {
        const auto selected =
            std::find_if(entries.begin(), entries.end(), [&selected_path](const auto& entry) {
                return entry.path == selected_path;
            });
        if (selected != entries.end()) {
            selected_index = static_cast<int>(std::distance(entries.begin(), selected));
        }
    }
    remote_revision.fetch_add(1, std::memory_order_relaxed);
}

void GameList::ToggleFavorite(u64 program_id) {
    if (program_id == 0) {
        return;
    }
    auto& favorite_ids = UWP::values.favorited_ids;
    const auto favorite = std::find(favorite_ids.begin(), favorite_ids.end(), program_id);
    const bool now_favorite = favorite == favorite_ids.end();
    if (now_favorite) {
        if (favorite_ids.size() >= MaxFavoriteTitles) {
            management_message = "The favorites list is full.";
            return;
        }
        favorite_ids.push_back(program_id);
    } else {
        favorite_ids.erase(favorite);
    }

    if (details_entry && details_entry->program_id == program_id) {
        details_entry->favorite = now_favorite;
    }
    {
        std::lock_guard lock{entries_mutex};
        for (auto& entry : entries) {
            if (entry.program_id == program_id) {
                entry.favorite = now_favorite;
            }
        }
    }
    SortEntries();
    UWP::SaveConfig();
    management_message = now_favorite ? "Added to favorites." : "Removed from favorites.";
}

std::optional<ConfigureRequest> GameList::TakeConfigureRequest() {
    std::optional<ConfigureRequest> request = std::move(configure_request);
    configure_request.reset();
    return request;
}

void GameList::StartPlayTimeTracking(u64 program_id) {
    if (program_id == 0) {
        return;
    }
    StopPlayTimeTracking();
    tracked_program_id = program_id;
    play_time_manager.SetProgramId(program_id);
    play_time_manager.Start();
    tracking_play_time = true;
}

void GameList::StopPlayTimeTracking() {
    if (!tracking_play_time) {
        return;
    }
    play_time_manager.Stop();
    const u64 play_time = play_time_manager.GetPlayTime(tracked_program_id);
    {
        std::lock_guard lock{entries_mutex};
        for (auto& entry : entries) {
            if (entry.program_id == tracked_program_id) {
                entry.play_time_seconds = play_time;
            }
        }
    }
    tracking_play_time = false;
    tracked_program_id = 0;
}

void GameList::OpenDetails(const GameListEntry& entry) {
    details_entry = entry;
    details_open = true;
    metadata_editor_open = false;
    focus_details_action = true;
    pending_action = ManagementAction::None;
    managed_path_label.clear();
    managed_path.clear();
    management_message.clear();
}

void GameList::OpenMetadataEditor(const GameListEntry& entry) {
    ++metadata_editor_session;
    metadata_editor_open = true;
    metadata_match_browser_open = false;
    focus_metadata_editor_first = true;
    metadata_save_prompt_requested = false;
    focus_metadata_search = false;
    focus_first_metadata_match = false;
    metadata_match_accept_ready = false;
    metadata_match_artwork_override = -1;
    metadata_match_artwork_override_game_id = 0;
    metadata_search_pending = false;
    metadata_field = MetadataField::None;
    metadata_field_buffer.clear();
    metadata_search_query.clear();
    metadata_selected_match = -1;
    metadata_thumbnail_index = -1;
    metadata_thumbnail_game_id = 0;
    metadata_preview_game_id = 0;
    metadata_match_preview.reset();
    metadata_editor_artwork = entry.box_art;
    metadata_preview_message.clear();
    metadata_matches.clear();
    metadata_match_thumbnails.clear();
    metadata_thumbnail_attempted.clear();
    metadata_match_row_focus_amount.clear();
    metadata_editor_message.clear();
    edited_metadata = entry.scraped.value_or(ScreenScraper::GameMetadata{});
    edited_metadata.program_id = entry.program_id;
    if (edited_metadata.name.empty()) {
        edited_metadata.name = entry.title;
    }
    edited_metadata.rating = RatingForEditor(std::move(edited_metadata.rating));
    original_metadata = edited_metadata;
    metadata_search_text = edited_metadata.name;
}

void GameList::CloseMetadataEditor() {
    ++metadata_editor_session;
    metadata_editor_open = false;
    metadata_match_browser_open = false;
    metadata_match_accept_ready = false;
    metadata_match_artwork_override = -1;
    metadata_match_artwork_override_game_id = 0;
    metadata_save_prompt_requested = false;
    metadata_field = MetadataField::None;
    metadata_selected_match = -1;
    metadata_thumbnail_index = -1;
    metadata_thumbnail_game_id = 0;
    metadata_match_preview.reset();
    metadata_editor_artwork.reset();
    metadata_match_thumbnails.clear();
    metadata_thumbnail_attempted.clear();
    metadata_match_row_focus_amount.clear();
    metadata_preview_message.clear();
    focus_details_action = true;
}

bool GameList::MetadataHasChanges() const {
    return !MetadataEqual(edited_metadata, original_metadata);
}

void GameList::ApplyMetadata(const ScreenScraper::GameMetadata& metadata) {
    if (details_entry && details_entry->program_id == metadata.program_id) {
        ApplyCachedMetadata(*details_entry);
    }
    {
        std::lock_guard lock{entries_mutex};
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& item) {
            return item.program_id == metadata.program_id;
        });
        if (entry != entries.end()) {
            ApplyCachedMetadata(*entry);
        }
    }
    icon_prune_requested = true;
    remote_revision.fetch_add(1, std::memory_order_relaxed);
}

bool GameList::ApplyEditedMetadata() {
    if (!details_entry) {
        return false;
    }
    edited_metadata.program_id = details_entry->program_id;
    std::string error;
    if (!ScreenScraper::SaveMetadata(edited_metadata, error)) {
        metadata_editor_message = std::move(error);
        return false;
    }
    edited_metadata.manually_edited = true;
    ApplyMetadata(edited_metadata);
    original_metadata = edited_metadata;
    metadata_editor_message = "Saved metadata for this title.";
    return true;
}

void GameList::OpenMetadataFieldEditor(MetadataField field) {
    metadata_field = field;
    focus_metadata_field_input = true;
    switch (field) {
    case MetadataField::Title:
        metadata_field_buffer = edited_metadata.name;
        break;
    case MetadataField::Description:
        metadata_field_buffer = edited_metadata.synopsis;
        break;
    case MetadataField::ReleaseDate:
        metadata_field_buffer = edited_metadata.release_date;
        break;
    case MetadataField::Developer:
        metadata_field_buffer = edited_metadata.developer;
        break;
    case MetadataField::Publisher:
        metadata_field_buffer = edited_metadata.publisher;
        break;
    case MetadataField::Genre:
        metadata_field_buffer = edited_metadata.genre;
        break;
    case MetadataField::Players:
        metadata_field_buffer = edited_metadata.players;
        break;
    case MetadataField::None:
        return;
    }
}

void GameList::StartMetadataSearch() {
    if (!details_entry || metadata_search_future.valid()) {
        return;
    }
    const bool has_query =
        std::count_if(metadata_search_text.begin(), metadata_search_text.end(),
                      [](unsigned char character) { return !std::isspace(character); }) >= 2;
    if (!has_query) {
        metadata_matches.clear();
        metadata_match_row_focus_amount.clear();
        metadata_match_artwork_override = -1;
        metadata_match_artwork_override_game_id = 0;
        metadata_selected_match = -1;
        metadata_editor_message = "Type at least two characters to search.";
        metadata_search_pending = false;
        return;
    }
    metadata_search_pending = false;
    metadata_search_query = metadata_search_text;
    metadata_matches.clear();
    metadata_match_thumbnails.clear();
    metadata_thumbnail_attempted.clear();
    metadata_match_row_focus_amount.clear();
    metadata_match_artwork_override = -1;
    metadata_match_artwork_override_game_id = 0;
    metadata_selected_match = -1;
    metadata_thumbnail_index = -1;
    metadata_thumbnail_game_id = 0;
    metadata_match_preview.reset();
    metadata_preview_game_id = 0;
    metadata_preview_message.clear();
    metadata_editor_message = "Searching ScreenScraper...";
    metadata_search_program_id = details_entry->program_id;
    metadata_search_session = metadata_editor_session;
    metadata_search_future = std::async(
        std::launch::async, [query = metadata_search_query,
                             preferred_region = ScreenScraperRegion(details_entry->region),
                             path = details_entry->path] {
            return ScreenScraper::SearchByTitle(query, preferred_region, path);
        });
}

void GameList::StartMetadataMatchDownload(const ScreenScraper::SearchMatch& match) {
    if (!details_entry || metadata_scrape_future.valid() || match.game_id == 0) {
        return;
    }
    ScreenScraper::ScrapeRequest request;
    request.program_id = details_entry->program_id;
    request.title = match.name.empty() ? metadata_search_text : match.name;
    request.path = details_entry->path;
    request.size_bytes = details_entry->size_bytes;
    request.preferred_region = ScreenScraperRegion(details_entry->region);
    request.screenscraper_game_id = match.game_id;
    request.screenscraper_system_id = match.system_id;
    request.force_metadata = true;
    request.persist_metadata = false;
    metadata_scrape_program_id = details_entry->program_id;
    metadata_scrape_session = metadata_editor_session;
    metadata_editor_message = "Downloading metadata and artwork for the selected match...";
    metadata_scrape_future = std::async(std::launch::async, [request = std::move(request)] {
        return ScreenScraper::Scrape(request, nullptr);
    });
}

void GameList::ResetMetadataMatchArtworkOverride() {
    metadata_match_artwork_override = -1;
    metadata_match_artwork_override_game_id = 0;
    metadata_match_artwork_load_pending = false;
    metadata_match_artwork_cache_game_id = 0;
    metadata_match_artwork_cache.clear();
    metadata_match_artwork_attempted.clear();
    metadata_preview_game_id = 0;
    metadata_preview_download_game_id = 0;
    metadata_preview_download_url.clear();
    metadata_match_preview.reset();
    metadata_preview_message.clear();
}

void GameList::CycleMetadataMatchArtwork(int direction) {
    if (direction == 0 || metadata_selected_match < 0 ||
        metadata_selected_match >= static_cast<int>(metadata_matches.size())) {
        return;
    }
    const auto& match = metadata_matches[static_cast<std::size_t>(metadata_selected_match)];
    const int first = static_cast<int>(ScreenScraperBoxArt::Box2D);
    const int last = static_cast<int>(ScreenScraperBoxArt::Support2D);
    const int count = last - first + 1;
    int current = metadata_match_artwork_override_game_id == match.game_id
                      ? metadata_match_artwork_override
                      : static_cast<int>(UWP::values.screenscraper_large_box_art.GetValue());
    if (current < first || current > last) {
        current = first;
    }
    for (int step = 1; step <= count; ++step) {
        int index = (current - first + direction * step) % count;
        if (index < 0) {
            index += count;
        }
        const auto candidate = static_cast<ScreenScraperBoxArt>(first + index);
        if (ScreenScraper::SearchArtworkUrl(match, candidate).empty()) {
            continue;
        }
        metadata_match_artwork_override = static_cast<int>(candidate);
        metadata_match_artwork_override_game_id = match.game_id;
        metadata_preview_game_id = 0;
        metadata_match_artwork_load_pending = true;
        metadata_match_artwork_changed_at = std::chrono::steady_clock::now();
        const std::string& url = ScreenScraper::SearchArtworkUrl(match, candidate);
        const auto cached = metadata_match_artwork_cache.find(url);
        if (metadata_match_artwork_cache_game_id == match.game_id &&
            cached != metadata_match_artwork_cache.end()) {
            metadata_match_preview = cached->second;
            metadata_preview_game_id = match.game_id;
            metadata_match_artwork_load_pending = false;
            metadata_preview_message.clear();
        } else {
            metadata_preview_message =
                fmt::format("Loading {}...", ScreenScraperArtworkName(candidate));
        }
        return;
    }
    metadata_preview_message = "No alternative artwork is available for this match.";
}

void GameList::PollMetadataOperations() {
    using namespace std::chrono_literals;
    if (metadata_search_future.valid() &&
        metadata_search_future.wait_for(0ms) == std::future_status::ready) {
        try {
            ScreenScraper::SearchResult result = metadata_search_future.get();
            if (details_entry && details_entry->program_id == metadata_search_program_id &&
                metadata_editor_open && metadata_match_browser_open &&
                metadata_search_session == metadata_editor_session &&
                metadata_search_query == metadata_search_text) {
                metadata_editor_message = std::move(result.message);
                metadata_matches = std::move(result.matches);
                metadata_match_row_focus_amount.clear();
                metadata_match_artwork_override = -1;
                metadata_match_artwork_override_game_id = 0;
                metadata_match_thumbnails.resize(metadata_matches.size());
                metadata_thumbnail_attempted.assign(metadata_matches.size(), false);
                metadata_selected_match = metadata_matches.empty() ? -1 : 0;
                metadata_match_preview.reset();
                metadata_preview_game_id = 0;
                metadata_preview_message.clear();
                focus_first_metadata_match = !metadata_matches.empty();
                metadata_match_accept_ready = false;
            }
        } catch (const std::exception& exception) {
            if (details_entry && details_entry->program_id == metadata_search_program_id &&
                metadata_editor_open && metadata_match_browser_open &&
                metadata_search_session == metadata_editor_session) {
                metadata_editor_message =
                    std::string{"ScreenScraper search failed: "} + exception.what();
            }
        } catch (...) {
            if (details_entry && details_entry->program_id == metadata_search_program_id &&
                metadata_editor_open && metadata_match_browser_open &&
                metadata_search_session == metadata_editor_session) {
                metadata_editor_message = "ScreenScraper search failed unexpectedly.";
            }
        }
        metadata_search_program_id = 0;
    }

    if (metadata_thumbnail_future.valid() &&
        metadata_thumbnail_future.wait_for(0ms) == std::future_status::ready) {
        try {
            ScreenScraper::ImageResult result = metadata_thumbnail_future.get();
            const bool current_thumbnail =
                metadata_editor_open && metadata_match_browser_open &&
                metadata_thumbnail_session == metadata_editor_session &&
                metadata_thumbnail_index >= 0 &&
                metadata_thumbnail_index < static_cast<int>(metadata_matches.size()) &&
                metadata_thumbnail_index < static_cast<int>(metadata_match_thumbnails.size()) &&
                metadata_matches[static_cast<std::size_t>(metadata_thumbnail_index)].game_id ==
                    metadata_thumbnail_game_id;
            if (current_thumbnail && result.success) {
                metadata_match_thumbnails[static_cast<std::size_t>(metadata_thumbnail_index)] =
                    LoadArtwork(result.bytes, metadata_thumbnail_game_id ^ 0x53535448554D424EULL);
            }
        } catch (const std::exception& exception) {
            LOG_WARNING(Frontend, "ScreenScraper match thumbnail failed: {}", exception.what());
        } catch (...) {
            LOG_WARNING(Frontend, "ScreenScraper match thumbnail failed unexpectedly");
        }
        metadata_thumbnail_index = -1;
        metadata_thumbnail_game_id = 0;
    }

    if (metadata_preview_future.valid() &&
        metadata_preview_future.wait_for(0ms) == std::future_status::ready) {
        try {
            ScreenScraper::ImageResult result = metadata_preview_future.get();
            const bool current_cache =
                metadata_editor_open && metadata_match_browser_open &&
                metadata_preview_session == metadata_editor_session &&
                metadata_match_artwork_cache_game_id == metadata_preview_download_game_id &&
                !metadata_preview_download_url.empty();
            std::shared_ptr<const GameListArtwork> artwork;
            if (current_cache && result.success) {
                artwork = LoadArtwork(result.bytes,
                                      metadata_preview_download_game_id ^
                                          std::hash<std::string>{}(metadata_preview_download_url));
                if (artwork) {
                    metadata_match_artwork_cache[metadata_preview_download_url] = artwork;
                }
            }
            if (current_cache && metadata_selected_match >= 0 &&
                metadata_selected_match < static_cast<int>(metadata_matches.size())) {
                const auto& selected =
                    metadata_matches[static_cast<std::size_t>(metadata_selected_match)];
                const bool artwork_override =
                    metadata_match_artwork_override_game_id == selected.game_id;
                const auto override_type =
                    static_cast<ScreenScraperBoxArt>(metadata_match_artwork_override);
                const std::string& desired_url =
                    artwork_override ? ScreenScraper::SearchArtworkUrl(selected, override_type)
                                     : selected.large_box_art_url;
                if (selected.game_id == metadata_preview_download_game_id &&
                    desired_url == metadata_preview_download_url) {
                    metadata_preview_message = std::move(result.message);
                    metadata_match_preview = std::move(artwork);
                    metadata_preview_game_id = selected.game_id;
                    if (result.success && !metadata_match_preview) {
                        metadata_preview_message = "The match artwork could not be decoded.";
                    }
                }
            }
        } catch (const std::exception& exception) {
            if (metadata_preview_session == metadata_editor_session) {
                metadata_preview_message =
                    std::string{"ScreenScraper preview failed: "} + exception.what();
            }
        } catch (...) {
            if (metadata_preview_session == metadata_editor_session) {
                metadata_preview_message = "ScreenScraper preview failed unexpectedly.";
            }
        }
        metadata_preview_download_game_id = 0;
        metadata_preview_download_url.clear();
    }

    if (metadata_scrape_future.valid() &&
        metadata_scrape_future.wait_for(0ms) == std::future_status::ready) {
        try {
            ScreenScraper::Result result = metadata_scrape_future.get();
            const bool current_editor =
                details_entry && details_entry->program_id == metadata_scrape_program_id &&
                metadata_editor_open && metadata_scrape_session == metadata_editor_session;
            if (current_editor) {
                metadata_editor_message = result.message;
            }
            if (result.success && result.metadata) {
                if (current_editor) {
                    edited_metadata = *result.metadata;
                    edited_metadata.rating = RatingForEditor(std::move(edited_metadata.rating));
                    metadata_editor_artwork =
                        LoadArtwork(ScreenScraper::SelectedBoxArtPath(edited_metadata));
                    metadata_editor_message =
                        metadata_match_browser_open
                            ? fmt::format("Using {}. Choose another entry to replace it, or press "
                                          "B to review.",
                                          edited_metadata.name)
                            : "Loaded the selected match. Review the fields, then save your "
                              "changes.";
                }
            }
        } catch (const std::exception& exception) {
            if (details_entry && details_entry->program_id == metadata_scrape_program_id &&
                metadata_editor_open) {
                metadata_editor_message =
                    std::string{"ScreenScraper download failed: "} + exception.what();
            }
        } catch (...) {
            if (details_entry && details_entry->program_id == metadata_scrape_program_id &&
                metadata_editor_open) {
                metadata_editor_message = "ScreenScraper download failed unexpectedly.";
            }
        }
        metadata_scrape_program_id = 0;
    }
}

void GameList::SelectManagedPath(std::string label, std::string path, bool create) {
    managed_path_label = std::move(label);
    managed_path = std::move(path);
    if (managed_path.empty()) {
        management_message = managed_path_label + " is unavailable for this title.";
        return;
    }
    if (create && !FileUtil::CreateFullPath(managed_path)) {
        management_message = "Could not prepare " + managed_path_label + '.';
        return;
    }
    management_message = FileUtil::Exists(managed_path)
                             ? managed_path_label + " is ready."
                             : managed_path_label + " does not exist yet.";
}

void GameList::StartUninstall(Service::FS::MediaType media_type, u64 title_id, std::string label) {
    if (management_future.valid()) {
        return;
    }
    if (!IsSafeUninstallTarget(media_type, title_id)) {
        management_message = "Refusing to uninstall an invalid or non-user title.";
        return;
    }
    management_message = "Uninstalling " + label + "...";
    management_future =
        std::async(std::launch::async, [media_type, title_id, label = std::move(label)]() mutable {
            const Result result = Service::AM::UninstallProgram(media_type, title_id);
            if (result.IsError()) {
                return ManagementResult{
                    false, false,
                    fmt::format("Failed to uninstall {} (error 0x{:08X}).", label, result.raw)};
            }
            return ManagementResult{true, true, "Uninstalled " + label + '.'};
        });
}

void GameList::StartUninstallEverything(const GameListEntry& entry) {
    if (management_future.valid()) {
        return;
    }

    struct Target {
        Service::FS::MediaType media_type;
        u64 title_id;
        std::string label;
    };
    std::vector<Target> targets;
    const auto add_if_safe = [&targets](Service::FS::MediaType media_type, u64 title_id,
                                        std::string label) {
        if (IsSafeUninstallTarget(media_type, title_id)) {
            targets.push_back(Target{media_type, title_id, std::move(label)});
        }
    };

    if (entry.media_type == Service::FS::MediaType::SDMC && IsUserApplication(entry.program_id)) {
        add_if_safe(entry.media_type, entry.program_id, "application");
    }
    add_if_safe(Service::FS::MediaType::SDMC, Service::AM::GetTitleUpdateId(entry.program_id),
                "update");
    add_if_safe(Service::FS::MediaType::SDMC, GetDlcTitleId(entry.program_id), "DLC");

    if (targets.empty()) {
        management_message = "No safely uninstallable application, update, or DLC was found.";
        return;
    }

    management_message = "Uninstalling application content, update, and DLC...";
    management_future = std::async(std::launch::async, [targets = std::move(targets)] {
        std::vector<std::string> removed;
        std::vector<std::string> failed;
        for (const Target& target : targets) {
            const Result result = Service::AM::UninstallProgram(target.media_type, target.title_id);
            if (result.IsSuccess()) {
                removed.push_back(target.label);
            } else {
                failed.push_back(fmt::format("{} (0x{:08X})", target.label, result.raw));
            }
        }

        std::string message;
        if (!removed.empty()) {
            message = "Uninstalled ";
            for (std::size_t i = 0; i < removed.size(); ++i) {
                if (i != 0) {
                    message += i + 1 == removed.size() ? " and " : ", ";
                }
                message += removed[i];
            }
            message += ". Save data was retained.";
        }
        if (!failed.empty()) {
            if (!message.empty()) {
                message += ' ';
            }
            message += "Failed to uninstall ";
            for (std::size_t i = 0; i < failed.size(); ++i) {
                if (i != 0) {
                    message += ", ";
                }
                message += failed[i];
            }
            message += '.';
        }
        return ManagementResult{failed.empty(), !removed.empty(), std::move(message)};
    });
}

void GameList::StartRomFsDump(const GameListEntry& entry, bool dump_update) {
    if (management_future.valid()) {
        return;
    }
    if (entry.program_id == 0 || !IsApplication(entry.program_id) || entry.encrypted ||
        entry.path.empty() || !FileUtil::Exists(entry.path)) {
        management_message = "This title does not have readable application content to dump.";
        return;
    }

    const std::string base_path = fmt::format(
        "{}romfs/{:016X}", FileUtil::GetUserPath(FileUtil::UserPath::DumpDir), entry.program_id);
    const std::string update_path =
        fmt::format("{}romfs/{:016X}", FileUtil::GetUserPath(FileUtil::UserPath::DumpDir),
                    Service::AM::GetTitleUpdateId(entry.program_id));
    management_message = "Dumping RomFS. Large titles can take several minutes...";
    management_future = std::async(
        std::launch::async,
        [source = entry.path, base_path, update_path, dump_update]() -> ManagementResult {
            const auto loader = Loader::GetLoader(source);
            if (!loader) {
                return {false, false, "Could not open the application loader for RomFS dumping."};
            }

            const Loader::ResultStatus base_result = loader->DumpRomFS(base_path);
            if (base_result != Loader::ResultStatus::Success) {
                return {false, false,
                        fmt::format("Could not dump base RomFS: {}. Output path: {}",
                                    LoaderResultName(base_result), base_path)};
            }

            if (!dump_update) {
                return {true, false,
                        fmt::format("Dumped base RomFS to {}. No installed update was found.",
                                    base_path)};
            }

            const Loader::ResultStatus update_result = loader->DumpUpdateRomFS(update_path);
            if (update_result != Loader::ResultStatus::Success) {
                return {false, false,
                        fmt::format("Base RomFS dumped to {}. Update RomFS failed: {}. Update "
                                    "output path: {}",
                                    base_path, LoaderResultName(update_result), update_path)};
            }
            return {true, false,
                    fmt::format("Dumped base RomFS to {} and update RomFS to {}.", base_path,
                                update_path)};
        });
}

void GameList::ExecuteManagementAction() {
    if (!details_entry) {
        pending_action = ManagementAction::None;
        return;
    }
    // Metadata scanning opens the files uninstall removes; join it before any mutation.
    CancelScan();
    const GameListEntry entry = *details_entry;

    switch (pending_action) {
    case ManagementAction::DeleteShaderCache: {
        bool found = false;
        bool success = true;
        for (const auto& path : GetOpenGLShaderCachePaths(entry.program_id)) {
            if (!FileUtil::Exists(path)) {
                continue;
            }
            found = true;
            success &= FileUtil::Delete(path);
        }
        if (!found) {
            management_message = "No OpenGL shader cache exists for this title.";
        } else {
            management_message = success ? "Deleted the OpenGL shader cache."
                                         : "Some OpenGL shader cache files could not be deleted.";
        }
        break;
    }
    case ManagementAction::ResetPlayTime: {
        play_time_manager.ResetProgramPlayTime(entry.program_id);
        details_entry->play_time_seconds = 0;
        {
            std::lock_guard lock{entries_mutex};
            for (auto& item : entries) {
                if (item.program_id == entry.program_id) {
                    item.play_time_seconds = 0;
                }
            }
        }
        management_message = "Reset the recorded play time.";
        break;
    }
    case ManagementAction::DumpRomFS: {
        const bool has_update =
            IsApplication(entry.program_id) &&
            IsSafeUninstallTarget(Service::FS::MediaType::SDMC,
                                  Service::AM::GetTitleUpdateId(entry.program_id));
        StartRomFsDump(entry, has_update);
        break;
    }
    case ManagementAction::UninstallApplication:
        StartUninstall(entry.media_type, entry.program_id, "the application");
        break;
    case ManagementAction::UninstallUpdate:
        StartUninstall(Service::FS::MediaType::SDMC,
                       Service::AM::GetTitleUpdateId(entry.program_id), "the update");
        break;
    case ManagementAction::UninstallDlc:
        StartUninstall(Service::FS::MediaType::SDMC, GetDlcTitleId(entry.program_id), "the DLC");
        break;
    case ManagementAction::UninstallEverything:
        StartUninstallEverything(entry);
        break;
    case ManagementAction::None:
        break;
    }
    pending_action = ManagementAction::None;
}

void GameList::PollManagementOperation() {
    using namespace std::chrono_literals;
    if (!management_future.valid() ||
        management_future.wait_for(0ms) != std::future_status::ready) {
        return;
    }

    const ManagementResult result = management_future.get();
    management_message = result.message;
    if (result.refresh_library) {
        details_open = false;
        details_entry.reset();
        selected_index = -1;
        Refresh();
    }
}

void GameList::DrawManagementConfirmation() {
    if (!ImGui::BeginPopupModal("Confirm title management", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    switch (pending_action) {
    case ManagementAction::DeleteShaderCache:
        ImGui::TextWrapped(
            "Delete this title's OpenGL shader cache? It will be rebuilt the next time the "
            "title runs.");
        break;
    case ManagementAction::ResetPlayTime:
        ImGui::TextWrapped("Reset the recorded play time for this title?");
        break;
    case ManagementAction::DumpRomFS:
        ImGui::TextWrapped(
            "Dump this title's base RomFS and its installed update RomFS, when present? "
            "Matching files in the title's dump folders may be overwritten. This can take "
            "several minutes.");
        break;
    case ManagementAction::UninstallApplication:
        ImGui::TextWrapped(
            "Uninstall this application content? Save data is kept. This cannot be undone.");
        break;
    case ManagementAction::UninstallUpdate:
        ImGui::TextWrapped("Uninstall this title's update? This cannot be undone.");
        break;
    case ManagementAction::UninstallDlc:
        ImGui::TextWrapped("Uninstall all DLC for this title? This cannot be undone.");
        break;
    case ManagementAction::UninstallEverything:
        ImGui::TextWrapped(
            "Uninstall this title's safely detected application content, update, and DLC? "
            "Save data is retained. Installed content cannot be recovered from Azahar.");
        break;
    case ManagementAction::None:
        ImGui::TextUnformatted("No action is pending.");
        break;
    }

    if (ImGui::Button("Confirm", ImVec2(180.0f, 0.0f))) {
        ExecuteManagementAction();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(180.0f, 0.0f)) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        pending_action = ManagementAction::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemDefaultFocus();
    ImGui::EndPopup();
}

void GameList::DrawMetadataFieldEditor() {
    if (metadata_field == MetadataField::None) {
        return;
    }

    const char* label = "Metadata";
    const char* hint = nullptr;
    std::size_t capacity = 512;
    bool multiline = false;
    switch (metadata_field) {
    case MetadataField::Title:
        label = "Title";
        hint = "Game title";
        break;
    case MetadataField::Description:
        label = "Description";
        hint = "Game description";
        capacity = 8192;
        multiline = true;
        break;
    case MetadataField::ReleaseDate:
        label = "Release date";
        hint = "YYYY-MM-DD";
        capacity = 64;
        break;
    case MetadataField::Developer:
        label = "Developer";
        capacity = 256;
        break;
    case MetadataField::Publisher:
        label = "Publisher";
        capacity = 256;
        break;
    case MetadataField::Genre:
        label = "Genre";
        capacity = 256;
        break;
    case MetadataField::Players:
        label = "Players";
        hint = "For example: 1 or 1-4";
        capacity = 64;
        break;
    case MetadataField::None:
        return;
    }

    const float popup_width =
        std::min(ImGui::GetMainViewport()->WorkSize.x * 0.82f, ImGui::GetFontSize() * 46.0f);
    const float popup_height =
        multiline ? ImGui::GetMainViewport()->WorkSize.y * 0.62f : ImGui::GetFontSize() * 10.0f;
    ImGui::SetNextWindowSize(ImVec2(popup_width, popup_height), ImGuiCond_Appearing);
    if (!ImGui::IsPopupOpen("Edit metadata field")) {
        ImGui::OpenPopup("Edit metadata field");
    }
    if (!ImGui::BeginPopupModal("Edit metadata field", nullptr,
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize)) {
        return;
    }

    ImGui::TextUnformatted(label);
    ImGui::Spacing();
    if (focus_metadata_field_input) {
        ImGui::SetKeyboardFocusHere();
    }
    if (multiline) {
        MetadataMultilineInput("##metadata_field_value", metadata_field_buffer, capacity,
                               ImVec2(-1.0f, -ImGui::GetFrameHeightWithSpacing() * 2.2f));
    } else {
        ImGui::SetNextItemWidth(-1.0f);
        MetadataTextInput("##metadata_field_value", metadata_field_buffer, capacity, hint);
    }
    if (focus_metadata_field_input) {
        ImGui::SetItemDefaultFocus();
        focus_metadata_field_input = false;
    }

    const float button_width =
        (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
    if (ImGui::Button("Apply", ImVec2(button_width, 0.0f))) {
        switch (metadata_field) {
        case MetadataField::Title:
            edited_metadata.name = metadata_field_buffer;
            break;
        case MetadataField::Description:
            edited_metadata.synopsis = metadata_field_buffer;
            break;
        case MetadataField::ReleaseDate:
            edited_metadata.release_date = metadata_field_buffer;
            break;
        case MetadataField::Developer:
            edited_metadata.developer = metadata_field_buffer;
            break;
        case MetadataField::Publisher:
            edited_metadata.publisher = metadata_field_buffer;
            break;
        case MetadataField::Genre:
            edited_metadata.genre = metadata_field_buffer;
            break;
        case MetadataField::Players:
            edited_metadata.players = metadata_field_buffer;
            break;
        case MetadataField::None:
            break;
        }
        metadata_field = MetadataField::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(button_width, 0.0f)) ||
        (!ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
        metadata_field = MetadataField::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void GameList::DrawMetadataMatchBrowser() {
    if (!metadata_match_browser_open || !details_entry) {
        return;
    }

    const bool searching = metadata_search_future.valid();
    const bool downloading = metadata_scrape_future.valid();
    Widgets::SetNextWindowFullArea();
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar;
    const ImVec4 transparent{0.0f, 0.0f, 0.0f, 0.0f};
    ImGui::PushStyleColor(ImGuiCol_WindowBg, transparent);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, transparent);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, transparent);
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, transparent);
    if (ImGui::Begin("Find ScreenScraper Match", nullptr, flags)) {
        if (!ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown)) {
            metadata_match_accept_ready = true;
        }
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            metadata_match_browser_open = false;
            focus_metadata_editor_first = true;
        }
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !downloading) {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, true)) {
                CycleMetadataMatchArtwork(-1);
            } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, true)) {
                CycleMetadataMatchArtwork(1);
            }
        }

        ImGui::TextWrapped("Find a match for %s", DisplayTitle(*details_entry).c_str());
        ImGui::Spacing();
        if (focus_metadata_search) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1.0f);
        const bool query_changed = MetadataTextInput("##metadata_search", metadata_search_text, 256,
                                                     "Type a game title...");
        const bool search_input_focused = ImGui::IsItemFocused() || ImGui::IsItemActive();
        if (focus_metadata_search) {
            ImGui::SetItemDefaultFocus();
            focus_metadata_search = false;
        }
        if (query_changed) {
            metadata_search_pending = true;
            metadata_search_changed_at = std::chrono::steady_clock::now();
            metadata_matches.clear();
            metadata_match_thumbnails.clear();
            metadata_thumbnail_attempted.clear();
            metadata_match_row_focus_amount.clear();
            metadata_match_artwork_override = -1;
            metadata_match_artwork_override_game_id = 0;
            metadata_selected_match = -1;
            metadata_thumbnail_index = -1;
            metadata_thumbnail_game_id = 0;
            metadata_match_preview.reset();
            metadata_preview_game_id = 0;
            metadata_preview_message.clear();
            metadata_editor_message = "Waiting for more typing...";
        }
        if (search_input_focused && ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false) &&
            !metadata_matches.empty()) {
            focus_first_metadata_match = true;
        }

        constexpr auto search_delay = std::chrono::milliseconds{200};
        if (metadata_search_pending && !searching &&
            std::chrono::steady_clock::now() - metadata_search_changed_at >= search_delay) {
            StartMetadataSearch();
        }

        if (!ScreenScraper::HasUserCredentials()) {
            ImGui::TextDisabled("Sign in under Configure > UI to search ScreenScraper.");
        } else if (!metadata_editor_message.empty()) {
            ImGui::TextWrapped("%s", metadata_editor_message.c_str());
        }
        if (metadata_selected_match >= 0 &&
            metadata_selected_match < static_cast<int>(metadata_matches.size())) {
            const auto& selected_match =
                metadata_matches[static_cast<std::size_t>(metadata_selected_match)];
            const auto artwork =
                metadata_match_artwork_override_game_id == selected_match.game_id
                    ? static_cast<ScreenScraperBoxArt>(metadata_match_artwork_override)
                    : ScreenScraperBoxArt::SmdhIcon;
            ImGui::TextDisabled("Artwork preview: %s  (LB/RB)", ScreenScraperArtworkName(artwork));
        }

        ImGui::Spacing();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float result_width = std::min(
            available.x * 0.60f, std::max(available.x * 0.46f, ImGui::GetFontSize() * 24.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Header, transparent);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, transparent);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, transparent);
        ImGui::PushStyleColor(ImGuiCol_NavCursor, transparent);
        if (ImGui::BeginChild("##metadata_match_results", ImVec2(result_width, available.y),
                              ImGuiChildFlags_NavFlattened,
                              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar)) {
            ImGui::TextDisabled("Matches");
            ImGui::Spacing();
            if (metadata_matches.empty() && !searching) {
                ImGui::TextDisabled("Matches will appear here as you type.");
            }
            const ImGuiStyle& result_style = ImGui::GetStyle();
            const float base_artwork_height =
                std::max(72.0f, ImGui::GetFontSize() * 3.4f) * kDefaultArtworkScale;
            const float row_padding = std::max(10.0f, ImGui::GetFontSize() * 0.4f);
            const float artwork_text_gap = std::max(12.0f, ImGui::GetFontSize() * 0.5f);
            const float base_row_height = base_artwork_height + row_padding * 2.0f;
            const float row_height = base_row_height * kFocusedRowScale;
            const bool results_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
                                         !ImGui::GetIO().WantTextInput;
            ImGui::BeginDisabled(downloading);
            for (std::size_t index = 0; index < metadata_matches.size(); ++index) {
                const auto& match = metadata_matches[index];
                const bool selected = metadata_selected_match == static_cast<int>(index);
                const bool artwork_override =
                    selected && metadata_match_artwork_override_game_id == match.game_id;
                const auto thumbnail = artwork_override
                                           ? metadata_match_preview
                                           : (index < metadata_match_thumbnails.size()
                                                  ? metadata_match_thumbnails[index]
                                                  : std::shared_ptr<const GameListArtwork>{});
                const std::string title =
                    !match.name.empty()
                        ? match.name
                        : (!metadata_search_text.empty()
                               ? metadata_search_text
                               : fmt::format("ScreenScraper game {}", match.game_id));
                std::string secondary = match.system;
                if (!match.release_date.empty()) {
                    if (!secondary.empty()) {
                        secondary += " - ";
                    }
                    secondary += match.release_date;
                }
                ImGui::PushID(static_cast<int>(index));
                if (focus_first_metadata_match && index == 0) {
                    ImGui::ClearActiveID();
                    ImGui::SetKeyboardFocusHere();
                }
                const ImGuiID row_id = ImGui::GetID("##match");
                const bool row_focused =
                    results_focused && ImGui::GetCurrentContext()->NavId == row_id;
                const float focus_scale = AdvanceRowFocusAnimation(
                    metadata_match_row_focus_amount, static_cast<int>(index), row_focused);
                const std::string action = selected ? "Use This Entry" : "";
                const bool pressed = ImGui::Selectable("##match", false, ImGuiSelectableFlags_None,
                                                       ImVec2(-1.0f, row_height));
                if (ImGui::IsItemFocused() || ImGui::IsItemHovered()) {
                    if (metadata_selected_match != static_cast<int>(index)) {
                        ResetMetadataMatchArtworkOverride();
                    }
                    metadata_selected_match = static_cast<int>(index);
                }
                if (ImGui::IsItemFocused() && index == 0 &&
                    ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false)) {
                    focus_metadata_search = true;
                }
                if (pressed) {
                    if (metadata_selected_match != static_cast<int>(index)) {
                        ResetMetadataMatchArtworkOverride();
                    }
                    metadata_selected_match = static_cast<int>(index);
                    if (metadata_match_accept_ready) {
                        metadata_match_accept_ready = false;
                        StartMetadataMatchDownload(match);
                    }
                }

                const ImVec2 row_min = ImGui::GetItemRectMin();
                const ImVec2 row_max = ImGui::GetItemRectMax();
                const float artwork_height = base_artwork_height * focus_scale;
                float content_x = row_min.x + row_padding * focus_scale;
                if (thumbnail) {
                    const unsigned int texture = GetOrCreateArtworkTexture(thumbnail);
                    if (texture != 0 && thumbnail->width > 0 && thumbnail->height > 0) {
                        const float draw_width = artwork_height *
                                                 static_cast<float>(thumbnail->width) /
                                                 static_cast<float>(thumbnail->height);
                        const float image_y =
                            row_min.y + (row_max.y - row_min.y - artwork_height) * 0.5f;
                        ImGui::GetWindowDrawList()->AddImage(
                            static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture)),
                            ImVec2(content_x, image_y),
                            ImVec2(content_x + draw_width, image_y + artwork_height));
                        content_x += draw_width + artwork_text_gap * focus_scale;
                    }
                }
                const float text_width =
                    std::max(1.0f, row_max.x - result_style.FramePadding.x - content_x);
                const float font_size = ImGui::GetFontSize() * focus_scale;
                const float title_height =
                    ImGui::CalcTextSize(title.c_str(), nullptr, false, text_width / focus_scale).y *
                    focus_scale;
                const float secondary_height = secondary.empty() ? 0.0f : font_size;
                const float action_height = action.empty() ? 0.0f : font_size;
                const float line_gap = result_style.FramePadding.y * focus_scale;
                const float text_block_height = title_height + secondary_height + action_height +
                                                (secondary.empty() ? 0.0f : line_gap) +
                                                (action.empty() ? 0.0f : line_gap);
                float next_text_y =
                    row_min.y + std::max(row_padding, (row_height - text_block_height) * 0.5f);
                auto* draw_list = ImGui::GetWindowDrawList();
                draw_list->PushClipRect(ImVec2(content_x, row_min.y), row_max, true);
                draw_list->AddText(ImGui::GetFont(), font_size, ImVec2(content_x, next_text_y),
                                   ImGui::GetColorU32(ImGuiCol_Text), title.c_str(), nullptr,
                                   text_width);
                if (!secondary.empty()) {
                    next_text_y += title_height + line_gap;
                    draw_list->AddText(ImGui::GetFont(), font_size, ImVec2(content_x, next_text_y),
                                       ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                       secondary.c_str());
                    next_text_y += secondary_height;
                } else {
                    next_text_y += title_height;
                }
                if (!action.empty()) {
                    next_text_y += line_gap;
                    draw_list->AddText(ImGui::GetFont(), font_size, ImVec2(content_x, next_text_y),
                                       ImGui::GetColorU32(ImGuiCol_Text), action.c_str());
                }
                draw_list->PopClipRect();
                ImGui::PopID();
            }
            ImGui::EndDisabled();
            focus_first_metadata_match = false;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(4);
        ImGui::SameLine();

        if (metadata_selected_match >= 0 &&
            metadata_selected_match < static_cast<int>(metadata_matches.size())) {
            const auto& match = metadata_matches[static_cast<std::size_t>(metadata_selected_match)];
            const auto selected_index = static_cast<std::size_t>(metadata_selected_match);
            if (metadata_match_artwork_cache_game_id != match.game_id) {
                metadata_match_artwork_cache_game_id = match.game_id;
                metadata_match_artwork_cache.clear();
                metadata_match_artwork_attempted.clear();
                metadata_match_artwork_load_pending = false;
                metadata_match_preview.reset();
                metadata_preview_game_id = 0;
            }
            if (!match.box_art_url.empty() && selected_index < metadata_match_thumbnails.size() &&
                metadata_match_thumbnails[selected_index]) {
                metadata_match_artwork_cache[match.box_art_url] =
                    metadata_match_thumbnails[selected_index];
            }
            const bool artwork_override = metadata_match_artwork_override_game_id == match.game_id;
            const auto override_type =
                static_cast<ScreenScraperBoxArt>(metadata_match_artwork_override);
            const std::string& preview_url =
                artwork_override ? ScreenScraper::SearchArtworkUrl(match, override_type)
                                 : match.large_box_art_url;
            const auto cached = metadata_match_artwork_cache.find(preview_url);
            if (!preview_url.empty() && cached != metadata_match_artwork_cache.end()) {
                metadata_match_preview = cached->second;
                metadata_preview_game_id = match.game_id;
                metadata_match_artwork_load_pending = false;
                metadata_preview_message.clear();
            } else if (preview_url.empty()) {
                metadata_match_artwork_load_pending = false;
                metadata_preview_message =
                    "The selected large artwork type is unavailable for this match.";
            } else {
                constexpr auto artwork_delay = std::chrono::milliseconds{120};
                const bool ready =
                    !metadata_match_artwork_load_pending ||
                    std::chrono::steady_clock::now() - metadata_match_artwork_changed_at >=
                        artwork_delay;
                if (!metadata_preview_future.valid() && ready &&
                    !metadata_match_artwork_attempted.contains(preview_url)) {
                    metadata_match_artwork_load_pending = false;
                    metadata_match_artwork_attempted.insert(preview_url);
                    metadata_preview_session = metadata_editor_session;
                    metadata_preview_download_game_id = match.game_id;
                    metadata_preview_download_url = preview_url;
                    metadata_preview_message = "Loading box art...";
                    metadata_preview_future = std::async(std::launch::async, [url = preview_url] {
                        return ScreenScraper::DownloadSearchPreview(url);
                    });
                } else if (!metadata_preview_future.valid() && ready &&
                           metadata_match_artwork_attempted.contains(preview_url)) {
                    metadata_preview_message = "The selected artwork could not be loaded.";
                }
            }
        }

        if (!metadata_thumbnail_future.valid() && !metadata_preview_future.valid()) {
            for (std::size_t index = 0; index < metadata_matches.size(); ++index) {
                if (index >= metadata_thumbnail_attempted.size() ||
                    metadata_thumbnail_attempted[index]) {
                    continue;
                }
                metadata_thumbnail_attempted[index] = true;
                const auto& match = metadata_matches[index];
                if (match.box_art_url.empty()) {
                    continue;
                }
                metadata_thumbnail_index = static_cast<int>(index);
                metadata_thumbnail_game_id = match.game_id;
                metadata_thumbnail_session = metadata_editor_session;
                metadata_thumbnail_future =
                    std::async(std::launch::async, [url = match.box_art_url] {
                        return ScreenScraper::DownloadSearchPreview(url, 180, 180);
                    });
                break;
            }
        }

        ImGui::PushStyleColor(ImGuiCol_ChildBg, transparent);
        ImGui::PushStyleColor(ImGuiCol_NavCursor, transparent);
        if (ImGui::BeginChild("##metadata_match_preview", ImVec2(0.0f, available.y),
                              ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_NoNavInputs |
                                  ImGuiWindowFlags_NoNavFocus)) {
            const float down = ImGui::GetKeyData(ImGuiKey_GamepadRStickDown)->AnalogValue;
            const float up = ImGui::GetKeyData(ImGuiKey_GamepadRStickUp)->AnalogValue;
            const float scroll_speed = ImGui::GetFontSize() * 21.0f * ImGui::GetIO().DeltaTime;
            ImGui::SetScrollY(ImGui::GetScrollY() + (down - up) * scroll_speed);

            if (metadata_selected_match >= 0 &&
                metadata_selected_match < static_cast<int>(metadata_matches.size())) {
                const auto& match =
                    metadata_matches[static_cast<std::size_t>(metadata_selected_match)];
                if (metadata_match_preview) {
                    const unsigned int texture = GetOrCreateArtworkTexture(metadata_match_preview);
                    if (texture != 0) {
                        const float source_width =
                            static_cast<float>(metadata_match_preview->width);
                        const float source_height =
                            static_cast<float>(metadata_match_preview->height);
                        const ImVec2 preview_available = ImGui::GetContentRegionAvail();
                        const float max_height = std::min(preview_available.y * 0.52f, 400.0f);
                        const float scale = std::min(preview_available.x / source_width,
                                                     max_height / source_height);
                        const ImVec2 image_size{source_width * scale, source_height * scale};
                        ImGui::SetCursorPosX(
                            ImGui::GetCursorPosX() +
                            std::max(0.0f, (preview_available.x - image_size.x) * 0.5f));
                        ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture)),
                                     image_size);
                        ImGui::Spacing();
                    }
                } else if (!metadata_preview_message.empty()) {
                    ImGui::TextDisabled("%s", metadata_preview_message.c_str());
                }

                ImGui::TextWrapped("%s", match.name.c_str());
                if (downloading) {
                    ImGui::TextDisabled("Downloading metadata and configured artwork...");
                }
                DrawFiveStarRating(match.rating);
                ImGui::Spacing();
                if (!match.synopsis.empty()) {
                    ImGui::TextWrapped("%s", match.synopsis.c_str());
                } else {
                    ImGui::TextDisabled("No description available.");
                }
                ImGui::Spacing();
                const auto field = [](const char* label, const std::string& value) {
                    if (!value.empty()) {
                        ImGui::TextDisabled("%s", label);
                        ImGui::SameLine();
                        ImGui::TextWrapped("%s", value.c_str());
                    }
                };
                field("Play time", FormatDuration(details_entry->play_time_seconds));
                field("Publisher", match.publisher);
                field("Developer", match.developer);
                field("Genre", match.genre);
                field("Released", match.release_date);
                field("Region", details_entry->region);
                field("Players", match.players);
                field("Compatibility", details_entry->compatibility);
                field("Format", details_entry->file_type);
                field("Size", FormatSize(details_entry->size_bytes));
            } else {
                ImGui::TextDisabled("Select a result to preview its metadata and box art.");
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
    }
    ImGui::End();
    ImGui::PopStyleColor(4);
}

void GameList::DrawMetadataEditor() {
    if (!metadata_editor_open || !details_entry) {
        return;
    }
    if (metadata_match_browser_open) {
        DrawMetadataMatchBrowser();
        return;
    }

    Widgets::SetNextWindowFullArea();
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar;
    const ImVec4 transparent{0.0f, 0.0f, 0.0f, 0.0f};
    ImGui::PushStyleColor(ImGuiCol_WindowBg, transparent);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, transparent);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, transparent);
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, transparent);
    if (ImGui::Begin("Edit Metadata", nullptr, flags)) {
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            if (MetadataHasChanges()) {
                metadata_save_prompt_requested = true;
            } else {
                CloseMetadataEditor();
            }
        }

        ImGui::TextWrapped("%s", DisplayTitle(*details_entry).c_str());
        ImGui::Spacing();
        const ImVec2 editor_available = ImGui::GetContentRegionAvail();
        const float editor_width =
            std::min(editor_available.x * 0.72f, ImGui::GetFontSize() * 58.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0.0f);
        if (ImGui::BeginChild("##metadata_editor", ImVec2(editor_width, editor_available.y),
                              ImGuiChildFlags_NavFlattened,
                              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar)) {
            const float down = ImGui::GetKeyData(ImGuiKey_GamepadRStickDown)->AnalogValue;
            const float up = ImGui::GetKeyData(ImGuiKey_GamepadRStickUp)->AnalogValue;
            const float scroll_speed = ImGui::GetFontSize() * 21.0f * ImGui::GetIO().DeltaTime;
            ImGui::SetScrollY(ImGui::GetScrollY() + (down - up) * scroll_speed);

            constexpr ImGuiTableFlags table_flags =
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
            if (ImGui::BeginTable("##metadata_fields", 2, table_flags, ImVec2(-1.0f, 0.0f))) {
                ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed,
                                        ImGui::GetFontSize() * 10.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                const auto row = [this](const char* label, const std::string& value,
                                        MetadataField field, bool focus = false) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", label);
                    ImGui::TableSetColumnIndex(1);
                    if (focus) {
                        ImGui::SetKeyboardFocusHere();
                    }
                    ImGui::PushID(static_cast<int>(field));
                    const std::string preview = MetadataRowPreview(value);
                    if (ImGui::Button(preview.c_str(), ImVec2(-1.0f, 0.0f))) {
                        OpenMetadataFieldEditor(field);
                    }
                    if (focus) {
                        ImGui::SetItemDefaultFocus();
                    }
                    ImGui::PopID();
                };
                row("Title", edited_metadata.name, MetadataField::Title,
                    focus_metadata_editor_first);
                focus_metadata_editor_first = false;
                row("Description", edited_metadata.synopsis, MetadataField::Description);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Rating");
                ImGui::TableSetColumnIndex(1);
                int rating = RatingValue(edited_metadata.rating);
                const int previous_rating = rating;
                const float rating_height = ImGui::GetFrameHeight();
                const bool rating_pressed =
                    ImGui::InvisibleButton("##metadata_rating", ImVec2(-1.0f, rating_height));
                const bool rating_focused = ImGui::IsItemFocused();
                if (rating_focused && ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true)) {
                    rating = std::max(0, rating - 1);
                }
                if (rating_focused && ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true)) {
                    rating = std::min(5, rating + 1);
                }
                if (rating_pressed) {
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        const float relative = ImGui::GetMousePos().x - ImGui::GetItemRectMin().x;
                        rating = std::clamp(static_cast<int>(std::ceil(
                                                relative / (ImGui::GetItemRectSize().x / 5.0f))),
                                            0, 5);
                    } else {
                        rating = (rating + 1) % 6;
                    }
                }
                const ImVec2 rating_min = ImGui::GetItemRectMin();
                if (rating_focused) {
                    ImGui::GetWindowDrawList()->AddRect(rating_min, ImGui::GetItemRectMax(),
                                                        ImGui::GetColorU32(ImGuiCol_NavCursor),
                                                        ImGui::GetStyle().FrameRounding, 0, 2.0f);
                }
                const ImVec2 stars_size = DrawStars(
                    ImGui::GetWindowDrawList(),
                    ImVec2(rating_min.x + ImGui::GetStyle().FramePadding.x,
                           rating_min.y + (rating_height - ImGui::GetFontSize() * 1.1f) * 0.5f),
                    rating);
                const std::string rating_text = fmt::format("{} / 5", rating);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(rating_min.x + stars_size.x + ImGui::GetStyle().ItemSpacing.x,
                           rating_min.y + (rating_height - ImGui::GetFontSize()) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_Text), rating_text.c_str());
                if (rating != previous_rating) {
                    edited_metadata.rating = fmt::format("{:.1f}", static_cast<float>(rating));
                }

                row("Release date", edited_metadata.release_date, MetadataField::ReleaseDate);
                row("Developer", edited_metadata.developer, MetadataField::Developer);
                row("Publisher", edited_metadata.publisher, MetadataField::Publisher);
                row("Genre", edited_metadata.genre, MetadataField::Genre);
                row("Players", edited_metadata.players, MetadataField::Players);
                ImGui::EndTable();
            }

            ImGui::Spacing();
            const bool can_search =
                ScreenScraper::HasDeveloperCredentials() && ScreenScraper::HasUserCredentials();
            ImGui::BeginDisabled(!can_search);
            if (ImGui::Button("Search ScreenScraper", ImVec2(-1.0f, 0.0f))) {
                metadata_match_browser_open = true;
                metadata_match_accept_ready = false;
                focus_metadata_search = true;
                metadata_search_text = edited_metadata.name;
                metadata_search_pending = true;
                metadata_search_changed_at =
                    std::chrono::steady_clock::now() - std::chrono::milliseconds{500};
                metadata_matches.clear();
                metadata_match_thumbnails.clear();
                metadata_thumbnail_attempted.clear();
                metadata_match_row_focus_amount.clear();
                metadata_match_artwork_override = -1;
                metadata_match_artwork_override_game_id = 0;
                metadata_selected_match = -1;
                metadata_thumbnail_index = -1;
                metadata_thumbnail_game_id = 0;
                metadata_match_preview.reset();
                metadata_preview_game_id = 0;
                metadata_editor_message.clear();
            }
            ImGui::EndDisabled();
            if (!can_search) {
                ImGui::TextDisabled("Sign in under Configure > UI to search ScreenScraper.");
            }

            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float action_width = (ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f;
            if (ImGui::Button("Save", ImVec2(action_width, 0.0f))) {
                if (ApplyEditedMetadata()) {
                    CloseMetadataEditor();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(action_width, 0.0f))) {
                if (MetadataHasChanges()) {
                    metadata_save_prompt_requested = true;
                } else {
                    CloseMetadataEditor();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset changes", ImVec2(action_width, 0.0f))) {
                edited_metadata = original_metadata;
                metadata_editor_artwork = details_entry->box_art;
                metadata_editor_message = "Reverted unsaved metadata changes.";
            }
            if (!metadata_editor_message.empty()) {
                ImGui::Spacing();
                ImGui::TextWrapped("%s", metadata_editor_message.c_str());
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##metadata_editor_artwork", ImVec2(0.0f, editor_available.y),
                              ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus |
                                  ImGuiWindowFlags_NoMouseInputs)) {
            ImGui::TextDisabled("Games-list box art");
            ImGui::Spacing();
            if (metadata_editor_artwork) {
                const unsigned int texture = GetOrCreateArtworkTexture(metadata_editor_artwork);
                if (texture != 0 && metadata_editor_artwork->width > 0 &&
                    metadata_editor_artwork->height > 0) {
                    const float source_width = static_cast<float>(metadata_editor_artwork->width);
                    const float source_height = static_cast<float>(metadata_editor_artwork->height);
                    const ImVec2 artwork_available = ImGui::GetContentRegionAvail();
                    const float max_width = artwork_available.x * 0.82f;
                    const float max_height =
                        std::min(artwork_available.y, ImGui::GetMainViewport()->WorkSize.y * 0.48f);
                    const float scale =
                        std::min(max_width / source_width, max_height / source_height);
                    const ImVec2 image_size{source_width * scale, source_height * scale};
                    ImGui::SetCursorPosX(
                        ImGui::GetCursorPosX() +
                        std::max(0.0f, (artwork_available.x - image_size.x) * 0.5f));
                    ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture)),
                                 image_size);
                }
            } else {
                ImGui::TextDisabled("No box art is available for this game.");
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        if (metadata_save_prompt_requested) {
            ImGui::OpenPopup("Save metadata changes");
            metadata_save_prompt_requested = false;
        }
        if (ImGui::BeginPopupModal("Save metadata changes", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextWrapped("Save the metadata changes for this title?");
            if (ImGui::Button("Save", ImVec2(180.0f, 0.0f))) {
                if (ApplyEditedMetadata()) {
                    CloseMetadataEditor();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard", ImVec2(180.0f, 0.0f))) {
                CloseMetadataEditor();
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::Button("Keep editing", ImVec2(368.0f, 0.0f)) ||
                (!ImGui::IsAnyItemActive() &&
                 ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        DrawMetadataFieldEditor();
    }
    ImGui::End();
    ImGui::PopStyleColor(4);
}

void GameList::DrawDetails() {
    if (!details_open || !details_entry) {
        return;
    }

    if (metadata_editor_open) {
        DrawMetadataEditor();
        return;
    }

    const GameListEntry& entry = *details_entry;
    const bool is_application = IsApplication(entry.program_id);
    const bool system_title =
        static_cast<u32>(entry.program_id >> 32) == static_cast<u32>(0x00040010);
    const u64 update_id = Service::AM::GetTitleUpdateId(entry.program_id);
    const u64 dlc_id = GetDlcTitleId(entry.program_id);
    const std::string update_path =
        Service::AM::GetTitlePath(Service::FS::MediaType::SDMC, update_id) + "content/";
    const std::string dlc_path =
        Service::AM::GetTitlePath(Service::FS::MediaType::SDMC, dlc_id) + "content/";
    const bool has_update = is_application && FileUtil::Exists(update_path);
    const bool has_dlc = is_application && FileUtil::Exists(dlc_path);
    const bool safely_installed_application =
        entry.media_type == Service::FS::MediaType::SDMC &&
        IsSafeUninstallTarget(entry.media_type, entry.program_id);
    const bool has_any_uninstall = safely_installed_application || has_update || has_dlc;
    const bool operation_active = management_future.valid();

    Widgets::SetNextWindowFullArea();
    if (focus_details_action) {
        ImGui::SetNextWindowFocus();
    }
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
    const ImVec4 transparent{0.0f, 0.0f, 0.0f, 0.0f};
    ImGui::PushStyleColor(ImGuiCol_WindowBg, transparent);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, transparent);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, transparent);
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, transparent);
    if (ImGui::Begin("Application Details", nullptr, flags)) {
        Widgets::HandleGamepadBack(details_open);

        ImGui::TextWrapped("%s", DisplayTitle(entry).c_str());
        ImGui::Spacing();
        const float action_width =
            std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 19.0f);
        if (ImGui::BeginChild("##details_actions", ImVec2(action_width, 0.0f),
                              ImGuiChildFlags_NavFlattened,
                              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar)) {
            ImGui::BeginDisabled(operation_active);
            ImGui::BeginDisabled(entry.program_id == 0);
            if (focus_details_action) {
                ImGui::SetKeyboardFocusHere();
                focus_details_action = false;
            }
            if (ImGui::Button(entry.favorite ? "Remove favorite" : "Add favorite",
                              ImVec2(-1.0f, 0.0f))) {
                ToggleFavorite(entry.program_id);
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!entry.can_insert);
            const bool cartridge_inserted =
                entry.can_insert && UWP::values.inserted_cartridge.GetValue() == entry.path;
            if (ImGui::Button(cartridge_inserted ? "Eject cartridge" : "Insert cartridge",
                              ImVec2(-1.0f, 0.0f))) {
                UWP::values.inserted_cartridge.SetValue(cartridge_inserted ? "" : entry.path);
                UWP::SaveConfig();
                management_message =
                    cartridge_inserted
                        ? "Cartridge ejected. The change applies to the next title launch."
                        : "Cartridge inserted. It will be visible to the next title launched.";
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Configure title", ImVec2(-1.0f, 0.0f))) {
                configure_request = ConfigureRequest{entry.path, entry.program_id};
                details_open = false;
            }
            if (ImGui::Button("Edit metadata", ImVec2(-1.0f, 0.0f))) {
                OpenMetadataEditor(entry);
            }

            if (ImGui::CollapsingHeader("Storage locations")) {
                const std::string sdmc_dir = FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir);
                const std::string save_path = FileSys::ArchiveSource_SDSaveData::GetSaveDataPathFor(
                    sdmc_dir, entry.program_id);
                const std::string extdata_path =
                    entry.extdata_id == 0
                        ? std::string{}
                        : FileSys::GetExtDataPathFromId(sdmc_dir, entry.extdata_id);
                const bool save_exists = is_application && FileUtil::Exists(save_path);
                const bool extdata_exists =
                    is_application && entry.extdata_id != 0 && FileUtil::Exists(extdata_path);
                const bool installed_media = entry.media_type != Service::FS::MediaType::GameCard;
                const std::string application_location =
                    installed_media && is_application
                        ? Service::AM::GetTitlePath(entry.media_type, entry.program_id) + "content/"
                        : entry.path;
                const auto path_button = [this](const char* label, const std::string& path,
                                                bool create, bool enabled = true) {
                    ImGui::BeginDisabled(!enabled);
                    if (ImGui::Button(label, ImVec2(-1.0f, 0.0f))) {
                        SelectManagedPath(label, path, create);
                    }
                    ImGui::EndDisabled();
                };
                path_button(installed_media ? "Application location" : "Application file",
                            application_location, false);
                path_button("Save data", save_path, false, save_exists);
                path_button("Extra data", extdata_path, false, extdata_exists);
                path_button("Mods",
                            fmt::format("{}{:016X}/",
                                        FileUtil::GetUserPath(FileUtil::UserPath::ModsDir),
                                        entry.program_id),
                            true, is_application);
                path_button("Custom textures",
                            fmt::format("{}textures/{:016X}/",
                                        FileUtil::GetUserPath(FileUtil::UserPath::LoadDir),
                                        entry.program_id),
                            true, is_application);
                path_button("Dumped textures",
                            fmt::format("{}textures/{:016X}/",
                                        FileUtil::GetUserPath(FileUtil::UserPath::DumpDir),
                                        entry.program_id),
                            true, is_application);
                path_button("Update data", update_path, false, has_update);
                path_button("DLC data", dlc_path, false, has_dlc);
                if (!managed_path.empty()) {
                    ImGui::TextDisabled("%s", managed_path_label.c_str());
                    ImGui::TextWrapped("%s", managed_path.c_str());
                }
            }

            if (ImGui::CollapsingHeader("Maintenance")) {
                ImGui::BeginDisabled(entry.program_id == 0 || !is_application || entry.encrypted);
                if (ImGui::Button("Dump RomFS", ImVec2(-1.0f, 0.0f))) {
                    pending_action = ManagementAction::DumpRomFS;
                    ImGui::OpenPopup("Confirm title management");
                }
                ImGui::EndDisabled();
                ImGui::BeginDisabled(!HasShaderCache(entry.program_id));
                if (ImGui::Button("Delete shader cache", ImVec2(-1.0f, 0.0f))) {
                    pending_action = ManagementAction::DeleteShaderCache;
                    ImGui::OpenPopup("Confirm title management");
                }
                ImGui::EndDisabled();
            }

            if (ImGui::CollapsingHeader("Uninstall")) {
                ImGui::BeginDisabled(!safely_installed_application || system_title);
                if (ImGui::Button("Uninstall application", ImVec2(-1.0f, 0.0f))) {
                    pending_action = ManagementAction::UninstallApplication;
                    ImGui::OpenPopup("Confirm title management");
                }
                ImGui::EndDisabled();
                ImGui::BeginDisabled(!has_update);
                if (ImGui::Button("Uninstall update", ImVec2(-1.0f, 0.0f))) {
                    pending_action = ManagementAction::UninstallUpdate;
                    ImGui::OpenPopup("Confirm title management");
                }
                ImGui::EndDisabled();
                ImGui::BeginDisabled(!has_dlc);
                if (ImGui::Button("Uninstall DLC", ImVec2(-1.0f, 0.0f))) {
                    pending_action = ManagementAction::UninstallDlc;
                    ImGui::OpenPopup("Confirm title management");
                }
                ImGui::EndDisabled();
                ImGui::BeginDisabled(!has_any_uninstall || system_title);
                if (ImGui::Button("Uninstall everything", ImVec2(-1.0f, 0.0f))) {
                    pending_action = ManagementAction::UninstallEverything;
                    ImGui::OpenPopup("Confirm title management");
                }
                ImGui::EndDisabled();
            }
            ImGui::EndDisabled();

            if (!management_message.empty()) {
                ImGui::TextWrapped("%s", management_message.c_str());
            }
            if (operation_active) {
                ImGui::TextDisabled("Working...");
            }

            DrawManagementConfirmation();
        }
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleColor(4);

    if (!details_open) {
        details_entry.reset();
        pending_action = ManagementAction::None;
    }
}

std::string GameList::Draw() {
    std::string launch_path;
    PollMetadataOperations();
    PollManagementOperation();
    if (details_open) {
        video_background->SetPath({});
        DrawDetails();
        return launch_path;
    }

    // A freshly opened library focuses its first visible title, never a menu button.
    const int current_frame = ImGui::GetFrameCount();
    if (last_home_frame + 1 != current_frame) {
        focus_first_game = true;
        top_menu_alpha = 1.0f;
        top_menu_last_active = std::chrono::steady_clock::now();
    }
    last_home_frame = current_frame;

    const int list_box_art_mode = static_cast<int>(UWP::values.screenscraper_box_art.GetValue());
    const int large_box_art_mode =
        static_cast<int>(UWP::values.screenscraper_large_box_art.GetValue());
    if (presented_list_box_art_mode != list_box_art_mode ||
        presented_large_box_art_mode != large_box_art_mode) {
        const bool changed_after_initial_load = presented_list_box_art_mode >= 0;
        bool selected_media_missing = false;
        {
            std::lock_guard lock{entries_mutex};
            for (auto& entry : entries) {
                ApplyCachedMetadata(entry);
                selected_media_missing |=
                    entry.scraped &&
                    (ScreenScraper::SelectedBoxArtPath(*entry.scraped).empty() ||
                     ScreenScraper::SelectedLargeBoxArtPath(*entry.scraped).empty());
            }
        }
        presented_list_box_art_mode = list_box_art_mode;
        presented_large_box_art_mode = large_box_art_mode;
        if (changed_after_initial_load && selected_media_missing && !scanning &&
            !scraping_metadata && UWP::values.screenscraper_enabled.GetValue() &&
            ScreenScraper::HasDeveloperCredentials() && ScreenScraper::HasUserCredentials()) {
            if (scrape_thread.joinable()) {
                scrape_thread.join();
            }
            stop_scanning = false;
            scraping_metadata = true;
            scrape_thread = std::thread(&GameList::ScrapeMissingMetadata, this, false);
        }
    }

    const auto background_mode = UWP::values.screenscraper_background.GetValue();
    const int background_mode_value = static_cast<int>(background_mode);
    if (presented_background_mode != background_mode_value) {
        const bool changed_after_initial_load = presented_background_mode >= 0;
        bool selected_media_missing = false;
        {
            std::lock_guard lock{entries_mutex};
            for (auto& entry : entries) {
                ApplyCachedMetadata(entry);
                selected_media_missing |=
                    entry.scraped && background_mode != ScreenScraperBackground::Off &&
                    ScreenScraper::SelectedBackgroundPath(*entry.scraped).empty();
            }
        }
        presented_background_mode = background_mode_value;

        // Fetch the newly selected media immediately rather than waiting for a rescan.
        if (changed_after_initial_load && selected_media_missing && !scanning &&
            !scraping_metadata && UWP::values.screenscraper_enabled.GetValue() &&
            ScreenScraper::HasDeveloperCredentials() && ScreenScraper::HasUserCredentials()) {
            if (scrape_thread.joinable()) {
                scrape_thread.join();
            }
            stop_scanning = false;
            scraping_metadata = true;
            scrape_thread = std::thread(&GameList::ScrapeMissingMetadata, this, false);
        }
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    std::shared_ptr<const GameListArtwork> selected_background;
    bool selected_background_is_video = false;
    std::string selected_video;
    {
        std::lock_guard lock{entries_mutex};
        if (selected_index >= 0 && selected_index < static_cast<int>(entries.size())) {
            const auto& selected = entries[static_cast<std::size_t>(selected_index)];
            selected_background = selected.background;
            if (UWP::values.screenscraper_background.GetValue() == ScreenScraperBackground::Video &&
                selected.scraped) {
                selected_video = selected.scraped->video_path;
            }
        }
    }
    video_background->SetPath(selected_video);
    if (!selected_video.empty()) {
        if (auto frame = video_background->CurrentFrame()) {
            selected_background = std::move(frame);
            selected_background_is_video = true;
        }
    }
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoSavedSettings;
    if (selected_background) {
        flags |= ImGuiWindowFlags_NoBackground;
    }

    if (!ImGui::Begin("##game_list", nullptr, flags)) {
        ImGui::End();
        return launch_path;
    }

    if (selected_background) {
        if (const unsigned int texture = GetOrCreateArtworkTexture(selected_background);
            texture != 0) {
            // Render behind the entire viewport rather than only the work area.
            const ImVec2 area_min = viewport->Pos;
            const ImVec2 area_max{area_min.x + viewport->Size.x, area_min.y + viewport->Size.y};
            const float source_aspect = static_cast<float>(selected_background->width) /
                                        static_cast<float>(selected_background->height);
            const float target_aspect = viewport->Size.x / viewport->Size.y;
            ImVec2 uv_min{0.0f, 0.0f};
            ImVec2 uv_max{1.0f, 1.0f};
            if (source_aspect > target_aspect) {
                const float visible = target_aspect / source_aspect;
                uv_min.x = (1.0f - visible) * 0.5f;
                uv_max.x = 1.0f - uv_min.x;
            } else {
                const float visible = source_aspect / target_aspect;
                uv_min.y = (1.0f - visible) * 0.5f;
                uv_max.y = 1.0f - uv_min.y;
            }
            // Media Foundation video rows are bottom-up and need reversed UVs; WIC images are
            // top-down.
            if (selected_background_is_video) {
                std::swap(uv_min.y, uv_max.y);
            }
            auto* draw_list = ImGui::GetBackgroundDrawList(viewport);
            draw_list->AddImage(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture)),
                                area_min, area_max, uv_min, uv_max);
            draw_list->AddRectFilled(area_min, area_max, IM_COL32(5, 8, 14, 190));
        }
    }

    // The menu is drawn as ordinary window content rather than through BeginMenuBar(), whose
    // separate nav layer can only be entered with the X toggle.
    if (menu_callback) {
        constexpr std::array<const char*, 6> menu_labels{
            "File", "Emulation", "View", "Multiplayer", "Tools", "Help",
        };
        std::array<ImGuiID, menu_labels.size()> menu_ids{};
        std::transform(menu_labels.begin(), menu_labels.end(), menu_ids.begin(),
                       [](const char* label) { return ImGui::GetID(label); });

        const auto menu_now = std::chrono::steady_clock::now();
        const ImGuiID previous_nav_id = ImGui::GetCurrentContext()->NavId;
        const bool menu_was_focused =
            std::find(menu_ids.begin(), menu_ids.end(), previous_nav_id) != menu_ids.end();
        const bool menu_popup_open =
            std::any_of(menu_labels.begin(), menu_labels.end(),
                        [](const char* label) { return ImGui::IsPopupOpen(label); });
        if (focus_file_menu || menu_was_focused || menu_popup_open) {
            top_menu_last_active = menu_now;
        }
        constexpr auto menu_visible_time = std::chrono::milliseconds{2200};
        constexpr float menu_fade_seconds = 0.35f;
        const float menu_target_alpha =
            menu_now - top_menu_last_active < menu_visible_time ? 1.0f : 0.0f;
        const float menu_fade_step = ImGui::GetIO().DeltaTime / menu_fade_seconds;
        if (top_menu_alpha < menu_target_alpha) {
            top_menu_alpha = std::min(menu_target_alpha, top_menu_alpha + menu_fade_step);
        } else {
            top_menu_alpha = std::max(menu_target_alpha, top_menu_alpha - menu_fade_step);
        }

        // SetKeyboardFocusHere targets the next item submitted, which is always File.
        if (focus_file_menu) {
            ImGui::SetKeyboardFocusHere();
            focus_file_menu = false;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * top_menu_alpha);
        menu_callback();
        ImGui::PopStyleVar();
        const ImGuiID nav_id = ImGui::GetCurrentContext()->NavId;
        const bool menu_button_focused =
            std::find(menu_ids.begin(), menu_ids.end(), nav_id) != menu_ids.end();
        if (menu_button_focused && ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false)) {
            focus_first_game = true;
        }
        ImGui::Spacing();
    }

    const std::string directory = UWP::values.game_directory.GetValue();
    if (directory.empty()) {
        search_requested = false;
        search_open = false;
        ImGui::Spacing();
        ImGui::TextWrapped("No application directory has been set.");
        ImGui::TextDisabled("Set one under Emulation > Configure > Storage, or place titles in "
                            "the Games folder under the active data root.");
        ImGui::End();
        return launch_path;
    }

    std::lock_guard lock{entries_mutex};
    const int icon_mode = std::clamp(UWP::values.game_list_icon_size.GetValue(), 0, 2);
    if (icon_mode != presented_icon_mode) {
        presented_icon_mode = icon_mode;
        icon_prune_requested = true;
    }
    if (icon_prune_requested.exchange(false)) {
        PruneIconTextures(icon_mode != 0);
    }

    if (search_requested) {
        ImGui::OpenPopup("Search games");
        search_requested = false;
    }
    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
    constexpr ImGuiWindowFlags search_flags =
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;
    if (ImGui::BeginPopupModal("Search games", nullptr, search_flags)) {
        std::array<char, 256> buffer{};
        const auto length = std::min(filter_text.size(), buffer.size() - 1);
        std::copy_n(filter_text.begin(), length, buffer.begin());
        ImGui::SetNextItemWidth(-1.0f);
        if (focus_search_input) {
            ImGui::SetKeyboardFocusHere();
        }
        GetVirtualKeyboard().InputText("##library_search", buffer.data(), buffer.size(),
                                       "Search titles, paths, regions, or IDs...");
        const bool search_input_focused = ImGui::IsItemFocused() || ImGui::IsItemActive();
        const bool focus_first_search_result =
            search_input_focused && ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false);
        if (focus_search_input) {
            ImGui::SetItemDefaultFocus();
            focus_search_input = false;
        }
        filter_text = buffer.data();

        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            filter_text.clear();
            search_open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::Spacing();
        const float search_list_inset = std::max(16.0f, ImGui::GetFontSize() * 0.6f);
        const float search_list_width =
            std::max(1.0f, ImGui::GetContentRegionAvail().x - search_list_inset);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + search_list_inset);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0.0f);
        const ImVec4 transparent{0.0f, 0.0f, 0.0f, 0.0f};
        ImGui::PushStyleColor(ImGuiCol_Header, transparent);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, transparent);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, transparent);
        ImGui::PushStyleColor(ImGuiCol_NavCursor, transparent);
        if (ImGui::BeginChild("##search_results", ImVec2(search_list_width, 0.0f),
                              ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar)) {
            std::vector<int> search_entries;
            search_entries.reserve(entries.size());
            const bool has_search_query =
                std::any_of(filter_text.begin(), filter_text.end(),
                            [](unsigned char character) { return !std::isspace(character); });
            if (has_search_query) {
                for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
                    const auto& entry = entries[static_cast<std::size_t>(i)];
                    if (MatchesFilter(entry)) {
                        search_entries.push_back(i);
                    }
                }
            }

            const bool show_box_art = UWP::values.screenscraper_enabled.GetValue();
            const ImGuiStyle& search_style = ImGui::GetStyle();
            const float base_artwork_height =
                show_box_art ? std::max(72.0f, ImGui::GetFontSize() * 3.4f) * kDefaultArtworkScale
                             : 0.0f;
            const float row_padding =
                show_box_art ? std::max(10.0f, ImGui::GetFontSize() * 0.4f) : 0.0f;
            const float artwork_text_gap = std::max(12.0f, ImGui::GetFontSize() * 0.5f);
            const float base_row_height =
                std::max(base_artwork_height + row_padding * 2.0f,
                         ImGui::CalcTextSize("Ag").y + search_style.FramePadding.y * 2.0f);
            const float row_height = base_row_height * kFocusedRowScale;
            const bool results_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
                                         !ImGui::GetIO().WantTextInput;

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(search_entries.size()), row_height);
            while (clipper.Step()) {
                for (int result = clipper.DisplayStart; result < clipper.DisplayEnd; ++result) {
                    const int i = search_entries[static_cast<std::size_t>(result)];
                    const auto& entry = entries[static_cast<std::size_t>(i)];
                    ImGui::PushID(i);
                    if (focus_first_search_result && result == 0) {
                        ImGui::ClearActiveID();
                        ImGui::SetKeyboardFocusHere();
                    }
                    const ImGuiID row_id = ImGui::GetID("##search_game");
                    const bool row_focused =
                        results_focused && ImGui::GetCurrentContext()->NavId == row_id;
                    const float focus_scale =
                        AdvanceRowFocusAnimation(search_row_focus_amount, i, row_focused);
                    if (ImGui::Selectable("##search_game", selected_index == i,
                                          ImGuiSelectableFlags_None, ImVec2(0.0f, row_height))) {
                        selected_index = i;
                        filter_text.clear();
                        search_open = false;
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::IsItemFocused()) {
                        selected_index = i;
                        if (result == 0 && ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false)) {
                            focus_search_input = true;
                        }
                    }

                    const ImVec2 row_min = ImGui::GetItemRectMin();
                    const ImVec2 row_max = ImGui::GetItemRectMax();
                    const float artwork_height = base_artwork_height * focus_scale;
                    float content_x =
                        row_min.x + (show_box_art ? row_padding * focus_scale
                                                  : search_style.FramePadding.x * focus_scale);
                    if (show_box_art && entry.box_art) {
                        const float draw_width = artwork_height *
                                                 static_cast<float>(entry.box_art->width) /
                                                 static_cast<float>(entry.box_art->height);
                        const unsigned int texture = GetOrCreateArtworkTexture(entry.box_art);
                        const float image_y =
                            row_min.y + (row_max.y - row_min.y - artwork_height) * 0.5f;
                        if (texture != 0) {
                            ImGui::GetWindowDrawList()->AddImage(
                                static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture)),
                                ImVec2(content_x, image_y),
                                ImVec2(content_x + draw_width, image_y + artwork_height));
                        }
                        content_x += draw_width + artwork_text_gap * focus_scale;
                    }

                    const std::string& title = DisplayTitle(entry);
                    const float title_height = ImGui::CalcTextSize(title.c_str()).y * focus_scale;
                    const float title_y = row_min.y + (row_max.y - row_min.y - title_height) * 0.5f;
                    auto* draw_list = ImGui::GetWindowDrawList();
                    draw_list->PushClipRect(ImVec2(content_x, row_min.y), row_max, true);
                    draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize() * focus_scale,
                                       ImVec2(content_x, title_y),
                                       ImGui::GetColorU32(ImGuiCol_Text), title.c_str());
                    draw_list->PopClipRect();
                    ImGui::PopID();
                }
            }
            if (has_search_query && search_entries.empty()) {
                ImGui::TextDisabled("No matching games.");
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    } else if (!search_requested) {
        search_open = false;
    }

    if (entries.empty() && !scanning) {
        ImGui::Spacing();
        ImGui::TextWrapped("No applications found in %s", directory.c_str());
        ImGui::End();
        return launch_path;
    }

    std::vector<int> visible_entries;
    visible_entries.reserve(entries.size());
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        if (MatchesFilter(entries[static_cast<std::size_t>(i)])) {
            visible_entries.push_back(i);
        }
    }
    if (!scanning && (selected_index < 0 || selected_index >= static_cast<int>(entries.size())) &&
        !visible_entries.empty()) {
        selected_index = visible_entries.front();
    }
    const bool has_selection =
        !scanning && selected_index >= 0 && selected_index < static_cast<int>(entries.size());
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 library_size = ImGui::GetContentRegionAvail();
    const float list_inset = std::max(16.0f, ImGui::GetFontSize() * 0.6f);
    const float detail_width =
        has_selection ? std::clamp(library_size.x * 0.32f, ImGui::GetFontSize() * 21.0f,
                                   ImGui::GetFontSize() * 31.0f)
                      : 0.0f;
    const float list_width =
        std::max(1.0f, (has_selection ? library_size.x - detail_width - style.ItemSpacing.x
                                      : library_size.x) -
                           list_inset);

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + list_inset);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0.0f);
    if (ImGui::BeginChild("##library_games", ImVec2(list_width, 0.0f),
                          ImGuiChildFlags_NavFlattened)) {
        const bool list_focused =
            ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput;
        if (list_focused && !visible_entries.empty() && selected_index == visible_entries.front() &&
            ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false)) {
            focus_file_menu = true;
        }
        if (list_focused && has_selection && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)) {
            OpenDetails(entries[static_cast<std::size_t>(selected_index)]);
        }
        if (list_focused && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) {
            OpenSearch();
        }
        constexpr ImGuiTableFlags table_flags =
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
        // Flatten the table's inner child so the list and the menu buttons share one nav scope.
        ImGuiContext* context = ImGui::GetCurrentContext();
        context->NextWindowData.HasFlags |= ImGuiNextWindowDataFlags_HasChildFlags;
        context->NextWindowData.ChildFlags |= ImGuiChildFlags_NavFlattened;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 0.0f));
        const ImVec4 transparent{0.0f, 0.0f, 0.0f, 0.0f};
        ImGui::PushStyleColor(ImGuiCol_Header, transparent);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, transparent);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, transparent);
        if (ImGui::BeginTable("##games", 1, table_flags)) {
            ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch);

            const bool show_box_art = UWP::values.screenscraper_enabled.GetValue();
            const float base_artwork_height =
                show_box_art ? std::max(72.0f, ImGui::GetFontSize() * 3.4f) * kDefaultArtworkScale
                             : 0.0f;
            const float row_padding =
                show_box_art ? std::max(10.0f, ImGui::GetFontSize() * 0.4f) : 0.0f;
            const float artwork_text_gap = std::max(12.0f, ImGui::GetFontSize() * 0.5f);
            const float text_block_height = ImGui::CalcTextSize("Ag").y;
            const float base_row_height = std::max(base_artwork_height + row_padding * 2.0f,
                                                   text_block_height + style.FramePadding.y * 2.0f);
            // Keep every row slot the same size while its contents animate.
            const float row_height = base_row_height * kFocusedRowScale;

            if (remote_focus_index >= 0) {
                // Scroll a remotely chosen row into the clipper's range so focus can land on it.
                const auto target =
                    std::find(visible_entries.begin(), visible_entries.end(), remote_focus_index);
                if (target == visible_entries.end()) {
                    remote_focus_index = -1;
                } else {
                    const float row_top =
                        static_cast<float>(std::distance(visible_entries.begin(), target)) *
                        row_height;
                    ImGui::SetScrollY(std::max(
                        0.0f, row_top - (ImGui::GetWindowHeight() - row_height) * 0.5f));
                }
            }

            // Upload only visible artwork.
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visible_entries.size()));
            while (clipper.Step()) {
                for (int visible_index = clipper.DisplayStart; visible_index < clipper.DisplayEnd;
                     ++visible_index) {
                    const int i = visible_entries[static_cast<std::size_t>(visible_index)];
                    const auto& entry = entries[static_cast<std::size_t>(i)];

                    ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
                    ImGui::PushID(i);
                    ImGui::TableSetColumnIndex(0);

                    const bool focus_this_game =
                        (focus_first_game && visible_index == 0) || i == remote_focus_index;
                    if (focus_this_game) {
                        // Called inside the table so the request lands on the first
                        // Selectable, not the table child.
                        ImGui::SetKeyboardFocusHere();
                    }

                    const ImGuiID row_id = ImGui::GetID("##game");
                    const bool row_focused = list_focused && context->NavId == row_id;
                    const float focus_scale =
                        AdvanceRowFocusAnimation(game_row_focus_amount, i, row_focused);
                    const bool selected = list_focused && selected_index == i;
                    std::string display_name = DisplayTitle(entry);
                    if (entry.favorite) {
                        display_name.insert(0, "[F] ");
                    }
                    if (entry.can_insert &&
                        UWP::values.inserted_cartridge.GetValue() == entry.path) {
                        display_name.insert(0, "[Cart] ");
                    }

                    if (ImGui::Selectable("##game", selected, ImGuiSelectableFlags_SpanAllColumns,
                                          ImVec2(0.0f, row_height))) {
                        selected_index = i;
                        launch_path = entry.path;
                    }
                    if (ImGui::IsItemFocused()) {
                        selected_index = i;
                    }
                    if (focus_this_game) {
                        selected_index = i;
                        focus_first_game = false;
                        remote_focus_index = -1;
                    }

                    const ImVec2 row_min = ImGui::GetItemRectMin();
                    const ImVec2 row_max = ImGui::GetItemRectMax();
                    const float rendered_row_height = row_max.y - row_min.y;
                    const float artwork_height = base_artwork_height * focus_scale;
                    float content_x =
                        row_min.x + (show_box_art ? row_padding * focus_scale
                                                  : style.FramePadding.x * focus_scale);
                    if (show_box_art && entry.box_art) {
                        const float draw_width = artwork_height *
                                                 static_cast<float>(entry.box_art->width) /
                                                 static_cast<float>(entry.box_art->height);
                        const unsigned int texture = GetOrCreateArtworkTexture(entry.box_art);
                        const float image_y =
                            row_min.y + (rendered_row_height - artwork_height) * 0.5f;
                        if (texture != 0) {
                            ImGui::GetWindowDrawList()->AddImage(
                                static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture)),
                                ImVec2(content_x, image_y),
                                ImVec2(content_x + draw_width, image_y + artwork_height));
                        }
                        content_x += draw_width + artwork_text_gap * focus_scale;
                    }

                    const float label_height =
                        ImGui::CalcTextSize(display_name.c_str()).y * focus_scale;
                    const float label_y = row_min.y + (rendered_row_height - label_height) * 0.5f;
                    const float name_clip_right = row_max.x - style.CellPadding.x;
                    auto* draw_list = ImGui::GetWindowDrawList();
                    draw_list->PushClipRect(ImVec2(content_x, row_min.y),
                                            ImVec2(name_clip_right, row_max.y), true);
                    draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize() * focus_scale,
                                       ImVec2(content_x, label_y),
                                       ImGui::GetColorU32(ImGuiCol_Text), display_name.c_str());
                    draw_list->PopClipRect();

                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    if (has_selection) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        if (ImGui::BeginChild("##selected_game", ImVec2(detail_width, 0.0f),
                              ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMouseInputs |
                                  ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus)) {
            // The right stick scrolls the details pane without moving focus.
            const float down = ImGui::GetKeyData(ImGuiKey_GamepadRStickDown)->AnalogValue;
            const float up = ImGui::GetKeyData(ImGuiKey_GamepadRStickUp)->AnalogValue;
            const float scroll_speed = ImGui::GetFontSize() * 21.0f * ImGui::GetIO().DeltaTime;
            ImGui::SetScrollY(ImGui::GetScrollY() + (down - up) * scroll_speed);

            const auto& entry = entries[static_cast<std::size_t>(selected_index)];
            const unsigned int artwork_texture =
                entry.large_box_art ? GetOrCreateArtworkTexture(entry.large_box_art) : 0;
            if (artwork_texture != 0) {
                const float source_width = static_cast<float>(entry.large_box_art->width);
                const float source_height = static_cast<float>(entry.large_box_art->height);
                const ImVec2 available = ImGui::GetContentRegionAvail();
                const float max_width = available.x;
                const float max_height = std::min(available.y * 0.52f, 400.0f);
                const float scale = std::min(max_width / source_width, max_height / source_height);
                const ImVec2 draw_size{source_width * scale, source_height * scale};
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                     std::max(0.0f, (available.x - draw_size.x) * 0.5f));
                ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(artwork_texture)),
                             draw_size);
                ImGui::Spacing();
            }

            ImGui::TextWrapped("%s", DisplayTitle(entry).c_str());
            if (scraping_metadata) {
                ImGui::TextColored(ImVec4(0.72f, 0.72f, 0.74f, 1.0f),
                                   "Updating artwork & descriptions...");
            }
            DrawFiveStarRating(entry.scraped ? entry.scraped->rating : std::string{});
            ImGui::Spacing();

            if (entry.scraped && !entry.scraped->synopsis.empty()) {
                ImGui::TextWrapped("%s", entry.scraped->synopsis.c_str());
            } else if (scraping_metadata) {
                ImGui::TextDisabled("Downloading description...");
            } else {
                ImGui::TextDisabled("No description available.");
            }
            ImGui::Spacing();

            const auto metadata_field = [](const char* label, const std::string& value) {
                if (value.empty()) {
                    return;
                }
                ImGui::TextDisabled("%s", label);
                ImGui::SameLine();
                ImGui::TextWrapped("%s", value.c_str());
            };
            metadata_field("Play time", FormatDuration(entry.play_time_seconds));
            if (entry.scraped) {
                metadata_field("Publisher", entry.scraped->publisher);
                metadata_field("Developer", entry.scraped->developer);
                metadata_field("Genre", entry.scraped->genre);
                metadata_field("Released", entry.scraped->release_date);
            }
            metadata_field("Region", entry.region);
            if (entry.scraped) {
                metadata_field("Players", entry.scraped->players);
            }
            metadata_field("Compatibility", entry.compatibility);
            metadata_field("Format", entry.file_type);
            metadata_field("Size", FormatSize(entry.size_bytes));
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
    }

    ImGui::End();
    return launch_path;
}

} // namespace UWP
