// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.

#include "citra_uwp/screenscraper.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <cryptopp/sha.h>
#include <fmt/format.h>
#include <json.hpp>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Credentials.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "citra_uwp/storage_paths.h"
#include "citra_uwp/uwp_settings.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "screenscraper_credentials.h"

namespace UWP::ScreenScraper {
namespace {

using nlohmann::json;
constexpr std::string_view kApiBase = "https://api.screenscraper.fr/api2/";
constexpr std::string_view kVaultResource = "Azahar.ScreenScraper";
constexpr int kNintendo3dsSystemId = 17;
constexpr int kNintendoEntertainmentSystemId = 3;
constexpr int kSuperNintendoSystemId = 4;
constexpr int kGameBoySystemId = 9;
constexpr int kGameBoyColorSystemId = 10;
constexpr int kGameBoyAdvanceSystemId = 12;
constexpr int kGameGearSystemId = 21;
constexpr int kPcEngineSystemId = 31;

struct HttpResult {
    int status = 0;
    std::vector<u8> body;
    std::string error;
};

std::string UrlEncode(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (const unsigned char character : value) {
        if (std::isalnum(character) || character == '-' || character == '_' || character == '.' ||
            character == '~') {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[character >> 4]);
            encoded.push_back(hex[character & 0x0f]);
        }
    }
    return encoded;
}

std::string AuthQuery() {
    std::string password;
    try {
        auto credentials = winrt::Windows::Security::Credentials::PasswordVault{}.FindAllByResource(
            winrt::to_hstring(kVaultResource));
        if (credentials.Size() != 0) {
            auto credential = credentials.GetAt(0);
            credential.RetrievePassword();
            password = winrt::to_string(credential.Password());
        }
    } catch (...) {
    }
    return "devid=" + UrlEncode(AZAHAR_SCREENSCRAPER_DEV_ID) +
           "&devpassword=" + UrlEncode(AZAHAR_SCREENSCRAPER_DEV_PASSWORD) +
           "&softname=" + UrlEncode(AZAHAR_SCREENSCRAPER_SOFTNAME) +
           "&output=json&ssid=" + UrlEncode(UWP::values.screenscraper_username.GetValue()) +
           "&sspassword=" + UrlEncode(password);
}

std::string TitleLookupUrl(std::string_view title, int system_id) {
    std::string url = std::string{kApiBase} + "jeuRecherche.php?" + AuthQuery() +
                      "&recherche=" + UrlEncode(title);
    if (system_id > 0) {
        url += "&systemeid=" + std::to_string(system_id);
    }
    return url;
}

std::string AlternateLeadingYearTitle(std::string_view title) {
    if (title.size() <= 5 || title[4] != ' ' ||
        !std::all_of(title.begin(), title.begin() + 4,
                     [](unsigned char character) { return std::isdigit(character) != 0; })) {
        return {};
    }
    return std::string{title.substr(5)} + " " + std::string{title.substr(0, 4)};
}

/// Maps a Virtual Console dump name, e.g. "(NES) (Virtual Console)", to its original system.
std::optional<int> VirtualConsoleSourceSystem(std::string_view path) {
    static constexpr std::array<std::pair<std::string_view, int>, 17> system_tags{{
        {"nes", kNintendoEntertainmentSystemId},
        {"famicom", kNintendoEntertainmentSystemId},
        {"snes", kSuperNintendoSystemId},
        {"super nintendo", kSuperNintendoSystemId},
        {"super famicom", kSuperNintendoSystemId},
        {"gb", kGameBoySystemId},
        {"game boy", kGameBoySystemId},
        {"gbc", kGameBoyColorSystemId},
        {"game boy color", kGameBoyColorSystemId},
        {"gba", kGameBoyAdvanceSystemId},
        {"game boy advance", kGameBoyAdvanceSystemId},
        {"gg", kGameGearSystemId},
        {"game gear", kGameGearSystemId},
        {"pce", kPcEngineSystemId},
        {"pc engine", kPcEngineSystemId},
        {"turbografx-16", kPcEngineSystemId},
        {"turbografx 16", kPcEngineSystemId},
    }};

    std::string lowercase_path{path};
    std::transform(
        lowercase_path.begin(), lowercase_path.end(), lowercase_path.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });

    bool virtual_console = false;
    std::optional<int> system_id;
    for (std::size_t open = lowercase_path.find('('); open != std::string::npos;) {
        const std::size_t close = lowercase_path.find(')', open);
        if (close == std::string::npos) {
            break;
        }
        // A tag may hold several comma-separated items.
        const std::string_view tag{lowercase_path.data() + open + 1, close - open - 1};
        for (const std::string& raw_item : Common::SplitString(tag, ',')) {
            const std::string item = Common::StripSpaces(raw_item);
            if (item == "virtual console") {
                virtual_console = true;
                continue;
            }
            const auto match = std::find_if(system_tags.begin(), system_tags.end(),
                                            [&](const auto& entry) { return entry.first == item; });
            if (match != system_tags.end()) {
                system_id = match->second;
            }
        }
        open = lowercase_path.find('(', close);
    }
    return virtual_console ? system_id : std::nullopt;
}

HttpResult Get(std::string_view url) {
    HttpResult result;
    try {
        // Stay below the base per-minute quota; downloads remain serial.
        static std::mutex request_mutex;
        static auto previous_request = std::chrono::steady_clock::time_point{};
        std::lock_guard request_lock{request_mutex};
        const auto now = std::chrono::steady_clock::now();
        const auto next_allowed = previous_request + std::chrono::milliseconds{1100};
        if (next_allowed > now) {
            std::this_thread::sleep_until(next_allowed);
        }
        previous_request = std::chrono::steady_clock::now();
        using namespace winrt::Windows::Web::Http;
        HttpClient client;
        client.DefaultRequestHeaders().UserAgent().ParseAdd(L"Azahar-UWP ScreenScraper client");
        const auto response =
            client.GetAsync(winrt::Windows::Foundation::Uri{winrt::to_hstring(std::string{url})})
                .get();
        result.status = static_cast<int>(response.StatusCode());
        const auto buffer = response.Content().ReadAsBufferAsync().get();
        auto reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(buffer);
        result.body.resize(buffer.Length());
        if (!result.body.empty()) {
            reader.ReadBytes(result.body);
        }
        if (!response.IsSuccessStatusCode()) {
            result.error = "ScreenScraper returned HTTP " + std::to_string(result.status);
        }
    } catch (const winrt::hresult_error& exception) {
        result.error = winrt::to_string(exception.message());
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

std::filesystem::path CacheDirectory(u64 program_id) {
    std::string storage_root = UWP::values.screenscraper_storage.GetValue();
    std::string error;
    if (!Storage::EnsureDirectory(storage_root, error)) {
        const std::string unavailable = storage_root;
        storage_root = std::string{Storage::DevelopmentScreenScraperPath};
        if (!Storage::EnsureDirectory(storage_root, error)) {
            storage_root = FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "screenscraper";
            Storage::EnsureDirectory(storage_root, error);
        }
        LOG_WARNING(Frontend, "ScreenScraper storage {} is unavailable; using {} for this launch",
                    unavailable, storage_root);
    }
    return std::filesystem::path{Common::UTF8ToUTF16(storage_root)} /
           Common::UTF8ToUTF16(fmt::format("{:016X}", program_id));
}

std::string Utf8Path(const std::filesystem::path& path) {
    return Common::UTF16ToUTF8(path.wstring());
}

u64 JsonU64(const json& node, const char* key) {
    const auto value = node.find(key);
    if (value == node.end()) {
        return 0;
    }
    if (value->is_number_unsigned()) {
        return value->get<u64>();
    }
    if (value->is_number_integer()) {
        return static_cast<u64>(std::max<s64>(0, value->get<s64>()));
    }
    if (value->is_string()) {
        u64 parsed = 0;
        const std::string text = value->get<std::string>();
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        return error == std::errc{} && end == text.data() + text.size() ? parsed : 0;
    }
    return 0;
}

std::string Localized(const json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_object()) {
        // Composite fields carry an `id` beside the localized names; visit those first.
        for (const char* key : {"text", "noms", "nom", "nom_en", "nom_us", "nomcourt", "nom_eu",
                                "nom_ss", "nom_fr"}) {
            if (const auto field = value.find(key); field != value.end()) {
                const std::string text = Localized(*field);
                if (!text.empty()) {
                    return text;
                }
            }
        }
        for (const auto& field : value) {
            const std::string text = Localized(field);
            if (!text.empty()) {
                return text;
            }
        }
    }
    if (value.is_array()) {
        for (const char* preferred : {"en", "us", "eu", "wor", "ss", "fr"}) {
            for (const auto& item : value) {
                if (!item.is_object()) {
                    continue;
                }
                const std::string region = item.value("region", item.value("langue", ""));
                if (region == preferred) {
                    const std::string text = Localized(item);
                    if (!text.empty()) {
                        return text;
                    }
                }
            }
        }
        for (const auto& item : value) {
            const std::string text = Localized(item);
            if (!text.empty()) {
                return text;
            }
        }
    }
    return {};
}

bool IsGameNode(const json& node) {
    return node.is_object() && JsonU64(node, "id") != 0 &&
           (node.contains("nom") || node.contains("noms") || node.contains("systeme") ||
            node.contains("medias") || node.contains("synopsis"));
}

const json* FindFirstGame(const json& value) {
    if (IsGameNode(value)) {
        return &value;
    }
    if (value.is_array()) {
        for (const auto& item : value) {
            if (const json* game = FindFirstGame(item)) {
                return game;
            }
        }
    } else if (value.is_object()) {
        // jeuRecherche returns either an array or an object keyed by result index.
        if (const auto nested = value.find("jeu"); nested != value.end()) {
            if (const json* game = FindFirstGame(*nested)) {
                return game;
            }
        }
        for (const auto& item : value) {
            if (const json* game = FindFirstGame(item)) {
                return game;
            }
        }
    }
    return nullptr;
}

const json* FindGame(const json& document) {
    const json* response = &document;
    if (const auto nested = response->find("response"); nested != response->end()) {
        response = &*nested;
    }
    if (const auto game = response->find("jeu"); game != response->end()) {
        return FindFirstGame(*game);
    }
    if (const auto games = response->find("jeux"); games != response->end()) {
        return FindFirstGame(*games);
    }
    return IsGameNode(*response) ? response : nullptr;
}

void CollectGames(const json& value, std::vector<const json*>& games) {
    if (IsGameNode(value)) {
        games.push_back(&value);
        return;
    }
    if (value.is_array()) {
        for (const auto& item : value) {
            CollectGames(item, games);
        }
    } else if (value.is_object()) {
        for (const auto& item : value) {
            CollectGames(item, games);
        }
    }
}

std::string Field(const json& game, const char* key) {
    const auto value = game.find(key);
    return value == game.end() ? std::string{} : Localized(*value);
}

std::string Sha1(const std::string& path, const std::atomic<bool>* cancelled) {
    std::ifstream input{std::filesystem::path{Common::UTF8ToUTF16(path)}, std::ios::binary};
    if (!input) {
        return {};
    }
    CryptoPP::SHA1 digest;
    // UWP worker threads have a small stack, so the read buffer lives on the heap.
    std::vector<u8> buffer(256 * 1024);
    while (input && (!cancelled || !cancelled->load())) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        digest.Update(buffer.data(), static_cast<std::size_t>(input.gcount()));
    }
    if (cancelled && cancelled->load()) {
        return {};
    }
    std::array<u8, CryptoPP::SHA1::DIGESTSIZE> bytes{};
    digest.Final(bytes.data());
    std::ostringstream text;
    text << std::uppercase << std::hex << std::setfill('0');
    for (u8 byte : bytes) {
        text << std::setw(2) << static_cast<unsigned>(byte);
    }
    return text.str();
}

json ToJson(const GameMetadata& value) {
    return {{"program_id", value.program_id},
            {"screenscraper_game_id", value.screenscraper_game_id},
            {"name", value.name},
            {"publisher", value.publisher},
            {"developer", value.developer},
            {"genre", value.genre},
            {"players", value.players},
            {"release_date", value.release_date},
            {"rating", value.rating},
            {"synopsis", value.synopsis},
            {"box_2d_path", value.box_2d_path},
            {"box_3d_path", value.box_3d_path},
            {"box_2d_back_path", value.box_2d_back_path},
            {"wheel_hd_path", value.wheel_hd_path},
            {"wheel_path", value.wheel_path},
            {"support_2d_path", value.support_2d_path},
            {"mix_v1_path", value.mix_v1_path},
            {"mix_v2_path", value.mix_v2_path},
            {"fanart_path", value.fanart_path},
            {"screenshot_path", value.screenshot_path},
            {"video_path", value.video_path},
            {"manually_edited", value.manually_edited}};
}

GameMetadata FromJson(const json& value) {
    GameMetadata metadata;
    metadata.program_id = value.value("program_id", u64{});
    metadata.screenscraper_game_id = value.value("screenscraper_game_id", u64{});
    metadata.name = value.value("name", "");
    metadata.publisher = value.value("publisher", "");
    metadata.developer = value.value("developer", "");
    metadata.genre = value.value("genre", "");
    metadata.players = value.value("players", "");
    metadata.release_date = value.value("release_date", "");
    metadata.rating = value.value("rating", "");
    metadata.synopsis = value.value("synopsis", "");
    metadata.box_2d_path = value.value("box_2d_path", "");
    metadata.box_3d_path = value.value("box_3d_path", "");
    metadata.box_2d_back_path = value.value("box_2d_back_path", "");
    metadata.wheel_hd_path = value.value("wheel_hd_path", "");
    metadata.wheel_path = value.value("wheel_path", "");
    metadata.support_2d_path = value.value("support_2d_path", "");
    metadata.mix_v1_path = value.value("mix_v1_path", "");
    metadata.mix_v2_path = value.value("mix_v2_path", "");
    metadata.fanart_path = value.value("fanart_path", "");
    metadata.screenshot_path = value.value("screenshot_path", "");
    metadata.video_path = value.value("video_path", "");
    metadata.manually_edited = value.value("manually_edited", false);
    return metadata;
}

bool WriteMetadata(const GameMetadata& metadata, std::string& error) {
    if (metadata.program_id == 0) {
        error = "This title has no stable program ID for the metadata cache.";
        return false;
    }
    const auto directory = CacheDirectory(metadata.program_id);
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    if (filesystem_error) {
        error = "Could not create the ScreenScraper cache directory.";
        return false;
    }

    const auto metadata_path = directory / L"metadata.json";
    const auto temporary = metadata_path.wstring() + L".tmp";
    {
        std::ofstream output{temporary, std::ios::trunc};
        output << ToJson(metadata).dump(2);
        if (!output) {
            error = "Could not write the metadata cache.";
            return false;
        }
    }
    std::filesystem::rename(temporary, metadata_path, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(metadata_path, filesystem_error);
        filesystem_error.clear();
        std::filesystem::rename(temporary, metadata_path, filesystem_error);
    }
    if (filesystem_error) {
        error = "Could not finish the metadata cache file.";
        return false;
    }
    return true;
}

std::optional<std::string> FindMediaUrl(const json& game, std::string_view type,
                                        std::string_view preferred_region = {}) {
    const auto medias = game.find("medias");
    if (medias == game.end()) {
        return std::nullopt;
    }

    struct MediaCandidate {
        std::string region;
        std::string url;
    };
    std::vector<MediaCandidate> candidates;
    const auto collect = [&](const json& media) {
        if (!media.is_object() || media.value("type", "") != type ||
            media.value("parent", "jeu") != "jeu") {
            return;
        }
        MediaCandidate candidate{media.value("region", ""), media.value("url", "")};
        if (candidate.url.empty()) {
            return;
        }
        std::transform(
            candidate.region.begin(), candidate.region.end(), candidate.region.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        candidates.push_back(std::move(candidate));
    };
    if (medias->is_array()) {
        for (const auto& media : *medias) {
            collect(media);
        }
    } else {
        collect(*medias);
    }
    if (candidates.empty()) {
        return std::nullopt;
    }

    std::string normalized_preference{preferred_region};
    std::transform(
        normalized_preference.begin(), normalized_preference.end(), normalized_preference.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    const auto find_region = [&](std::string_view region) -> std::optional<std::string> {
        const auto candidate =
            std::find_if(candidates.begin(), candidates.end(),
                         [&](const auto& item) { return item.region == region; });
        return candidate == candidates.end() ? std::nullopt : std::optional{candidate->url};
    };
    if (!normalized_preference.empty()) {
        if (auto url = find_region(normalized_preference)) {
            return url;
        }
    }
    for (const std::string_view region : {"wor", "us", "eu", "jp", "au", "ss"}) {
        if (region != normalized_preference) {
            if (auto url = find_region(region)) {
                return url;
            }
        }
    }
    return candidates.front().url;
}

std::optional<std::string> FindBoxArtUrl(const json& game, ScreenScraperBoxArt type,
                                         std::string_view preferred_region) {
    switch (type) {
    case ScreenScraperBoxArt::Box2D:
        return FindMediaUrl(game, "box-2D", preferred_region);
    case ScreenScraperBoxArt::Box3D:
        return FindMediaUrl(game, "box-3D", preferred_region);
    case ScreenScraperBoxArt::MixRecalboxV1:
        return FindMediaUrl(game, "mixrbv1", preferred_region);
    case ScreenScraperBoxArt::MixRecalboxV2:
        return FindMediaUrl(game, "mixrbv2", preferred_region);
    case ScreenScraperBoxArt::FanArt:
        return FindMediaUrl(game, "fanart", preferred_region);
    case ScreenScraperBoxArt::Screenshot:
        return FindMediaUrl(game, "ss", preferred_region);
    case ScreenScraperBoxArt::GameLogo:
        if (auto url = FindMediaUrl(game, "wheel-hd", preferred_region)) {
            return url;
        }
        return FindMediaUrl(game, "wheel", preferred_region);
    case ScreenScraperBoxArt::Box2DBack:
        return FindMediaUrl(game, "box-2D-back", preferred_region);
    case ScreenScraperBoxArt::Support2D:
        return FindMediaUrl(game, "support-2D", preferred_region);
    default:
        return std::nullopt;
    }
}

bool DownloadMedia(std::string_view url, std::string_view type,
                   const std::filesystem::path& destination, bool video,
                   const std::atomic<bool>* cancelled, std::string& error) {
    if (cancelled && cancelled->load()) {
        return false;
    }
    HttpResult response = Get(url);
    if (!response.error.empty() || response.body.empty()) {
        error =
            response.error.empty() ? "ScreenScraper returned an empty media file" : response.error;
        return false;
    }
    const bool valid_png =
        response.body.size() >= 8 && response.body[0] == 0x89 && response.body[1] == 'P' &&
        response.body[2] == 'N' && response.body[3] == 'G' && response.body[4] == 0x0D &&
        response.body[5] == 0x0A && response.body[6] == 0x1A && response.body[7] == 0x0A;
    // Box media is PNG, but screenshots and fan art are normally JPEG.
    const bool valid_jpeg = response.body.size() >= 3 && response.body[0] == 0xFF &&
                            response.body[1] == 0xD8 && response.body[2] == 0xFF;
    const bool valid_image = valid_png || valid_jpeg;
    const bool valid_video = response.body.size() >= 12 && response.body[4] == 'f' &&
                             response.body[5] == 't' && response.body[6] == 'y' &&
                             response.body[7] == 'p';
    if (video ? !valid_video : !valid_image) {
        const std::string reply{response.body.begin(),
                                response.body.begin() +
                                    std::min<std::size_t>(response.body.size(), 64)};
        error = "ScreenScraper returned no usable " + std::string{type} + " media";
        if (!reply.empty() && std::all_of(reply.begin(), reply.end(),
                                          [](unsigned char c) { return c >= 0x20 && c < 0x7f; })) {
            error += " (" + reply + ")";
        }
        return false;
    }
    const auto temporary = destination.wstring() + L".tmp";
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        output.write(reinterpret_cast<const char*>(response.body.data()), response.body.size());
        if (!output) {
            error = "Could not write downloaded media to the cache";
            return false;
        }
    }
    std::error_code filesystem_error;
    std::filesystem::rename(temporary, destination, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(destination, filesystem_error);
        filesystem_error.clear();
        std::filesystem::rename(temporary, destination, filesystem_error);
    }
    if (filesystem_error) {
        error = "Could not finish the cached media file";
        return false;
    }
    return true;
}

} // namespace

bool HasDeveloperCredentials() {
    return std::string_view{AZAHAR_SCREENSCRAPER_DEV_ID}.size() != 0 &&
           std::string_view{AZAHAR_SCREENSCRAPER_DEV_PASSWORD}.size() != 0;
}

bool SaveUserCredentials(const std::string& username, const std::string& password,
                         std::string& error) {
    if (username.empty() || password.empty()) {
        error = "Enter your ScreenScraper username and password.";
        return false;
    }
    try {
        ClearUserCredentials();
        winrt::Windows::Security::Credentials::PasswordVault{}.Add(
            winrt::Windows::Security::Credentials::PasswordCredential{
                winrt::to_hstring(kVaultResource), winrt::to_hstring(username),
                winrt::to_hstring(password)});
        UWP::values.screenscraper_username.SetValue(username);
        return true;
    } catch (const winrt::hresult_error& exception) {
        error = winrt::to_string(exception.message());
        return false;
    }
}

void ClearUserCredentials() {
    try {
        auto vault = winrt::Windows::Security::Credentials::PasswordVault{};
        const auto credentials = vault.FindAllByResource(winrt::to_hstring(kVaultResource));
        for (const auto& credential : credentials) {
            vault.Remove(credential);
        }
    } catch (...) {
    }
}

bool HasUserCredentials() {
    if (UWP::values.screenscraper_username.GetValue().empty()) {
        return false;
    }
    try {
        return winrt::Windows::Security::Credentials::PasswordVault{}
                   .FindAllByResource(winrt::to_hstring(kVaultResource))
                   .Size() != 0;
    } catch (...) {
        return false;
    }
}

Result TestLogin() {
    Result result;
    if (!HasDeveloperCredentials()) {
        result.message = "This build does not contain ScreenScraper developer credentials.";
        return result;
    }
    if (!HasUserCredentials()) {
        result.message = "No ScreenScraper user is signed in.";
        return result;
    }
    const HttpResult response = Get(std::string{kApiBase} + "ssuserInfos.php?" + AuthQuery());
    if (!response.error.empty()) {
        result.message = response.error;
        return result;
    }
    const json document = json::parse(response.body, nullptr, false);
    if (document.is_discarded()) {
        result.message = "ScreenScraper returned invalid account data.";
        return result;
    }
    const json* root = &document;
    if (const auto response_node = root->find("response"); response_node != root->end()) {
        root = &*response_node;
    }
    const auto user = root->find("ssuser");
    if (user == root->end()) {
        result.message = "ScreenScraper did not accept those account credentials.";
        return result;
    }
    result.success = true;
    result.authenticated = true;
    const auto used = JsonU64(*user, "requeststoday");
    const auto maximum = JsonU64(*user, "maxrequestsperday");
    result.message =
        "Signed in as " + user->value("id", UWP::values.screenscraper_username.GetValue()) +
        (maximum > 0
             ? " (daily requests " + std::to_string(used) + "/" + std::to_string(maximum) + ")"
             : "");
    return result;
}

std::optional<GameMetadata> LoadCached(u64 program_id) {
    if (program_id == 0) {
        return std::nullopt;
    }
    std::ifstream input{CacheDirectory(program_id) / L"metadata.json"};
    if (!input) {
        return std::nullopt;
    }
    const json document = json::parse(input, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return std::nullopt;
    }
    GameMetadata metadata = FromJson(document);
    const auto discard_invalid = [](std::string& path) {
        std::error_code error;
        if (path.empty() ||
            !std::filesystem::is_regular_file(std::filesystem::path{Common::UTF8ToUTF16(path)},
                                              error) ||
            std::filesystem::file_size(std::filesystem::path{Common::UTF8ToUTF16(path)}, error) <=
                16) {
            path.clear();
        }
    };
    discard_invalid(metadata.box_2d_path);
    discard_invalid(metadata.box_3d_path);
    discard_invalid(metadata.box_2d_back_path);
    discard_invalid(metadata.wheel_hd_path);
    discard_invalid(metadata.wheel_path);
    discard_invalid(metadata.support_2d_path);
    discard_invalid(metadata.mix_v1_path);
    discard_invalid(metadata.mix_v2_path);
    discard_invalid(metadata.fanart_path);
    discard_invalid(metadata.screenshot_path);
    discard_invalid(metadata.video_path);
    return metadata.program_id == program_id ? std::optional{std::move(metadata)} : std::nullopt;
}

const std::string& SearchArtworkUrl(const SearchMatch& match, ScreenScraperBoxArt type) {
    static const std::string empty;
    const int value = static_cast<int>(type);
    const int first = static_cast<int>(ScreenScraperBoxArt::Box2D);
    const int last = static_cast<int>(ScreenScraperBoxArt::Support2D);
    if (value < first || value > last) {
        return empty;
    }
    return match.artwork_urls[static_cast<std::size_t>(value - first)];
}

SearchResult SearchByTitle(std::string_view title, std::string_view preferred_region,
                           std::string_view path, const std::atomic<bool>* cancelled) {
    SearchResult result;
    if (!HasDeveloperCredentials()) {
        result.message = "The package has no ScreenScraper developer credentials.";
        return result;
    }
    if (!HasUserCredentials()) {
        result.message = "Sign in to ScreenScraper under Configure > UI.";
        return result;
    }
    if (title.empty()) {
        result.message = "Enter a title to search for.";
        return result;
    }
    if (cancelled && cancelled->load()) {
        result.message = "Search cancelled.";
        return result;
    }

    json document;
    std::vector<const json*> games;
    // Returns an error message; a search with no matches is not an error.
    const auto search = [&](int system_id) -> std::optional<std::string> {
        games.clear();
        const HttpResult response = Get(TitleLookupUrl(title, system_id));
        if (response.status == 404) {
            return std::nullopt;
        }
        if (!response.error.empty()) {
            return response.error + " while searching for " + std::string{title} + ".";
        }
        document = json::parse(response.body, nullptr, false);
        if (document.is_discarded()) {
            return "ScreenScraper returned invalid search data.";
        }
        const json* root = &document;
        if (const auto response_node = root->find("response"); response_node != root->end()) {
            root = &*response_node;
        }
        const json* games_node = root;
        if (const auto jeux = root->find("jeux"); jeux != root->end()) {
            games_node = &*jeux;
        } else if (const auto jeu = root->find("jeu"); jeu != root->end()) {
            games_node = &*jeu;
        }
        CollectGames(*games_node, games);
        return std::nullopt;
    };

    const auto source_system = VirtualConsoleSourceSystem(path);
    auto error = search(source_system.value_or(kNintendo3dsSystemId));
    if (!error && games.empty() && source_system) {
        error = search(kNintendo3dsSystemId);
    }
    if (error) {
        result.message = std::move(*error);
        return result;
    }
    std::vector<u64> seen_ids;
    for (const json* game : games) {
        const u64 game_id = JsonU64(*game, "id");
        if (game_id == 0 ||
            std::find(seen_ids.begin(), seen_ids.end(), game_id) != seen_ids.end()) {
            continue;
        }
        seen_ids.push_back(game_id);

        SearchMatch match;
        match.game_id = game_id;
        match.name = Field(*game, "noms");
        if (match.name.empty()) {
            match.name = Field(*game, "nom");
        }
        match.publisher = Field(*game, "editeur");
        match.developer = Field(*game, "developpeur");
        match.genre = Field(*game, "genres");
        match.players = Field(*game, "joueurs");
        match.release_date = Field(*game, "dates");
        match.rating = Field(*game, "note");
        match.synopsis = Field(*game, "synopsis");
        const int first_artwork = static_cast<int>(ScreenScraperBoxArt::Box2D);
        const int last_artwork = static_cast<int>(ScreenScraperBoxArt::Support2D);
        for (int value = first_artwork; value <= last_artwork; ++value) {
            const auto type = static_cast<ScreenScraperBoxArt>(value);
            if (auto media = FindBoxArtUrl(*game, type, preferred_region)) {
                match.artwork_urls[static_cast<std::size_t>(value - first_artwork)] =
                    std::move(*media);
            }
        }
        match.box_art_url = SearchArtworkUrl(match, UWP::values.screenscraper_box_art.GetValue());
        match.large_box_art_url =
            SearchArtworkUrl(match, UWP::values.screenscraper_large_box_art.GetValue());
        if (const auto system = game->find("systeme"); system != game->end()) {
            match.system_id = static_cast<int>(JsonU64(*system, "id"));
            match.system = Localized(*system);
        }
        if (match.system_id == 0) {
            match.system_id = kNintendo3dsSystemId;
        }
        result.matches.push_back(std::move(match));
    }

    result.success = true;
    result.message = result.matches.empty()
                         ? "No ScreenScraper matches were found."
                         : fmt::format("Found {} ScreenScraper match{}.", result.matches.size(),
                                       result.matches.size() == 1 ? "" : "es");
    return result;
}

ImageResult DownloadSearchPreview(std::string_view url, u32 max_width, u32 max_height,
                                  const std::atomic<bool>* cancelled) {
    ImageResult result;
    if (url.empty()) {
        result.message = "No preview artwork is available for this match.";
        return result;
    }
    if (cancelled && cancelled->load()) {
        result.message = "Preview cancelled.";
        return result;
    }

    std::string request_url{url};
    request_url += request_url.find('?') == std::string::npos ? '?' : '&';
    request_url +=
        "maxwidth=" + std::to_string(max_width) + "&maxheight=" + std::to_string(max_height);
    HttpResult response = Get(request_url);
    if (!response.error.empty() || response.body.empty()) {
        result.message = response.error.empty() ? "ScreenScraper returned an empty preview image."
                                                : std::move(response.error);
        return result;
    }
    const bool png =
        response.body.size() >= 8 && response.body[0] == 0x89 && response.body[1] == 'P' &&
        response.body[2] == 'N' && response.body[3] == 'G' && response.body[4] == 0x0D &&
        response.body[5] == 0x0A && response.body[6] == 0x1A && response.body[7] == 0x0A;
    const bool jpeg = response.body.size() >= 3 && response.body[0] == 0xFF &&
                      response.body[1] == 0xD8 && response.body[2] == 0xFF;
    if (!png && !jpeg) {
        result.message = "ScreenScraper returned an invalid preview image.";
        return result;
    }
    result.success = true;
    result.bytes = std::move(response.body);
    return result;
}

Result Scrape(const ScrapeRequest& request, const std::atomic<bool>* cancelled) {
    Result result;
    if (!HasDeveloperCredentials()) {
        result.message = "The package has no ScreenScraper developer credentials.";
        return result;
    }
    if (!HasUserCredentials()) {
        result.message = "Sign in to ScreenScraper under Configure > UI.";
        return result;
    }
    if (request.program_id == 0) {
        result.message = "This title has no stable program ID for the media cache.";
        return result;
    }

    const bool selected_match = request.screenscraper_game_id != 0;
    // Virtual Console titles are searched on their original system before the 3DS.
    const auto source_system = VirtualConsoleSourceSystem(request.path);
    const int title_search_system_id = source_system.value_or(kNintendo3dsSystemId);
    int title_match_system_id = request.screenscraper_system_id > 0
                                    ? request.screenscraper_system_id
                                    : title_search_system_id;
    const std::string title_lookup_url = TitleLookupUrl(request.title, title_search_system_id);
    std::string lookup_url;
    const bool regular_file =
        std::filesystem::is_regular_file(std::filesystem::path{Common::UTF8ToUTF16(request.path)});
    if (selected_match) {
        lookup_url = std::string{kApiBase} + "jeuInfos.php?" + AuthQuery() +
                     "&systemeid=" + std::to_string(title_match_system_id) +
                     "&gameid=" + std::to_string(request.screenscraper_game_id);
    } else if (regular_file) {
        const std::string sha1 = Sha1(request.path, cancelled);
        if (sha1.empty()) {
            result.message = cancelled && cancelled->load() ? "Scraping cancelled."
                                                            : "Could not hash the game file.";
            return result;
        }
        lookup_url =
            std::string{kApiBase} + "jeuInfos.php?" + AuthQuery() +
            "&systemeid=" + std::to_string(kNintendo3dsSystemId) + "&romtype=rom&romnom=" +
            UrlEncode(Common::UTF16ToUTF8(
                std::filesystem::path{Common::UTF8ToUTF16(request.path)}.filename().wstring())) +
            "&romtaille=" + std::to_string(request.size_bytes) + "&sha1=" + sha1;
    } else {
        lookup_url = title_lookup_url;
    }

    HttpResult response = Get(lookup_url);
    bool used_title_lookup = !selected_match && !regular_file;
    // A ROM missing from the hash catalogue returns 404; fall back to the name-search endpoint.
    if (!selected_match && regular_file && response.status == 404) {
        LOG_DEBUG(Frontend, "No ScreenScraper hash match for {}; trying title search",
                  request.title);
        response = Get(title_lookup_url);
        used_title_lookup = true;
    }
    if (!response.error.empty()) {
        result.message = response.error + " while looking up " + request.title +
                         (used_title_lookup ? " by title." : " by ROM hash.");
        return result;
    }
    json document = json::parse(response.body, nullptr, false);
    const json* game = document.is_discarded() ? nullptr : FindGame(document);
    if (!selected_match && regular_file && !used_title_lookup && (!game || !game->is_object())) {
        LOG_DEBUG(Frontend, "ScreenScraper hash response had no game for {}; trying title search",
                  request.title);
        response = Get(title_lookup_url);
        used_title_lookup = true;
        if (!response.error.empty()) {
            result.message = response.error + " while looking up " + request.title + " by title.";
            return result;
        }
        document = json::parse(response.body, nullptr, false);
        game = document.is_discarded() ? nullptr : FindGame(document);
    }
    const std::string alternate_title = AlternateLeadingYearTitle(request.title);
    const auto try_title_search = [&](std::string_view title, int system_id) {
        LOG_DEBUG(Frontend, "Trying ScreenScraper title search for {} on system {}", title,
                  system_id);
        response = Get(TitleLookupUrl(title, system_id));
        if (!response.error.empty()) {
            return false;
        }
        document = json::parse(response.body, nullptr, false);
        game = document.is_discarded() ? nullptr : FindGame(document);
        if (game && game->is_object()) {
            title_match_system_id = system_id;
            return true;
        }
        return false;
    };
    if (used_title_lookup && (!game || !game->is_object()) && !alternate_title.empty()) {
        try_title_search(alternate_title, title_search_system_id);
    }
    if (used_title_lookup && (!game || !game->is_object()) && source_system) {
        if (!try_title_search(request.title, kNintendo3dsSystemId) && !alternate_title.empty()) {
            try_title_search(alternate_title, kNintendo3dsSystemId);
        }
    }
    if (!response.error.empty()) {
        result.message = response.error + " while looking up " + request.title + " by title.";
        return result;
    }
    if (!game || !game->is_object()) {
        result.message = "No ScreenScraper match was found for " + request.title + ".";
        return result;
    }

    // A name-search hit may carry only an ID; resolve it through jeuInfos for the full object.
    if (used_title_lookup) {
        const u64 search_game_id = JsonU64(*game, "id");
        if (search_game_id == 0) {
            result.message = "ScreenScraper returned a search match without a game ID.";
            return result;
        }
        const std::string details_url = std::string{kApiBase} + "jeuInfos.php?" + AuthQuery() +
                                        "&systemeid=" + std::to_string(title_match_system_id) +
                                        "&gameid=" + std::to_string(search_game_id);
        response = Get(details_url);
        if (!response.error.empty()) {
            result.message = response.error + " while loading the ScreenScraper title match for " +
                             request.title + ".";
            return result;
        }
        document = json::parse(response.body, nullptr, false);
        game = document.is_discarded() ? nullptr : FindGame(document);
        if (!game || !game->is_object()) {
            result.message =
                "ScreenScraper could not load details for the title match " + request.title + ".";
            return result;
        }
    }

    GameMetadata metadata;
    metadata.program_id = request.program_id;
    metadata.screenscraper_game_id = JsonU64(*game, "id");
    metadata.name = Field(*game, "noms");
    metadata.publisher = Field(*game, "editeur");
    metadata.developer = Field(*game, "developpeur");
    metadata.genre = Field(*game, "genres");
    metadata.players = Field(*game, "joueurs");
    metadata.release_date = Field(*game, "dates");
    metadata.rating = Field(*game, "note");
    metadata.synopsis = Field(*game, "synopsis");
    const auto cached = LoadCached(request.program_id);
    if (cached) {
        metadata.box_2d_path = cached->box_2d_path;
        metadata.box_3d_path = cached->box_3d_path;
        metadata.box_2d_back_path = cached->box_2d_back_path;
        metadata.wheel_hd_path = cached->wheel_hd_path;
        metadata.wheel_path = cached->wheel_path;
        metadata.support_2d_path = cached->support_2d_path;
        metadata.mix_v1_path = cached->mix_v1_path;
        metadata.mix_v2_path = cached->mix_v2_path;
        metadata.fanart_path = cached->fanart_path;
        metadata.screenshot_path = cached->screenshot_path;
        metadata.video_path = cached->video_path;
    }
    if (cached && cached->manually_edited && !request.force_metadata) {
        metadata.name = cached->name;
        metadata.publisher = cached->publisher;
        metadata.developer = cached->developer;
        metadata.genre = cached->genre;
        metadata.players = cached->players;
        metadata.release_date = cached->release_date;
        metadata.rating = cached->rating;
        metadata.synopsis = cached->synopsis;
        metadata.manually_edited = true;
    } else if (!UWP::values.screenscraper_download_metadata.GetValue() && !request.force_metadata) {
        metadata.name.clear();
        metadata.publisher.clear();
        metadata.developer.clear();
        metadata.genre.clear();
        metadata.players.clear();
        metadata.release_date.clear();
        metadata.rating.clear();
        metadata.synopsis.clear();
    }
    if (metadata.screenscraper_game_id == 0) {
        result.message = "ScreenScraper returned a match without a game ID.";
        return result;
    }

    const auto directory = CacheDirectory(request.program_id);
    const auto media_directory =
        selected_match ? directory / std::to_wstring(metadata.screenscraper_game_id) : directory;
    std::error_code filesystem_error;
    std::filesystem::create_directories(media_directory, filesystem_error);
    if (filesystem_error) {
        result.message = "Could not create the ScreenScraper cache directory.";
        return result;
    }

    auto media = [&](bool enabled, std::string_view type, const wchar_t* filename,
                     std::string& path, bool video = false) {
        const auto destination = media_directory / filename;
        std::error_code size_error;
        if (std::filesystem::is_regular_file(destination) &&
            std::filesystem::file_size(destination, size_error) > 16) {
            path = Utf8Path(destination);
            return;
        }
        std::filesystem::remove(destination, size_error);
        path.clear();
        if (enabled) {
            const auto media_url = FindMediaUrl(*game, type, request.preferred_region);
            if (!media_url) {
                LOG_INFO(Frontend, "ScreenScraper lists no {} media for {}", type, request.title);
                return;
            }
            std::string ignored_error;
            if (DownloadMedia(*media_url, type, destination, video, cancelled, ignored_error)) {
                path = Utf8Path(destination);
            } else {
                LOG_WARNING(Frontend, "ScreenScraper {} download failed for {}: {}", type,
                            request.title, ignored_error);
            }
        }
    };
    media(UWP::values.screenscraper_download_box_2d.GetValue(), "box-2D", L"box-2d.png",
          metadata.box_2d_path);
    media(UWP::values.screenscraper_download_box_3d.GetValue(), "box-3D", L"box-3d.png",
          metadata.box_3d_path);
    media(UWP::values.screenscraper_download_box_2d_back.GetValue(), "box-2D-back",
          L"box-2d-back.png", metadata.box_2d_back_path);
    // The library always shows a logo.
    media(true, "wheel-hd", L"wheel-hd.png", metadata.wheel_hd_path);
    media(UWP::values.screenscraper_download_game_logo.GetValue() || metadata.wheel_hd_path.empty(),
          "wheel", L"wheel.png", metadata.wheel_path);
    media(UWP::values.screenscraper_download_support_2d.GetValue(), "support-2D", L"support-2d.png",
          metadata.support_2d_path);
    media(UWP::values.screenscraper_download_mix_v1.GetValue(), "mixrbv1", L"mix-v1.png",
          metadata.mix_v1_path);
    media(UWP::values.screenscraper_download_mix_v2.GetValue(), "mixrbv2", L"mix-v2.png",
          metadata.mix_v2_path);
    media(UWP::values.screenscraper_download_fanart.GetValue(), "fanart", L"fanart.jpg",
          metadata.fanart_path);
    media(UWP::values.screenscraper_download_screenshot.GetValue(), "ss", L"screenshot.jpg",
          metadata.screenshot_path);
    media(UWP::values.screenscraper_download_video.GetValue(), "video-normalized", L"video.mp4",
          metadata.video_path, true);

    if (request.persist_metadata) {
        std::string write_error;
        if (!WriteMetadata(metadata, write_error)) {
            result.message = std::move(write_error);
            return result;
        }
    }
    result.success = true;
    result.authenticated = true;
    result.message = "Downloaded ScreenScraper data for " + request.title + ".";
    result.metadata = std::move(metadata);
    return result;
}

bool SaveMetadata(GameMetadata metadata, std::string& error) {
    metadata.manually_edited = true;
    return WriteMetadata(metadata, error);
}

std::string BoxArtPath(const GameMetadata& metadata, ScreenScraperBoxArt type) {
    switch (type) {
    case ScreenScraperBoxArt::Box2D:
        return metadata.box_2d_path;
    case ScreenScraperBoxArt::Box3D:
        return metadata.box_3d_path;
    case ScreenScraperBoxArt::MixRecalboxV1:
        return metadata.mix_v1_path;
    case ScreenScraperBoxArt::MixRecalboxV2:
        return metadata.mix_v2_path;
    case ScreenScraperBoxArt::FanArt:
        return metadata.fanart_path;
    case ScreenScraperBoxArt::Screenshot:
        return metadata.screenshot_path;
    case ScreenScraperBoxArt::GameLogo:
        // Prefer the HD logo, falling back to the standard wheel.
        return !metadata.wheel_hd_path.empty() ? metadata.wheel_hd_path : metadata.wheel_path;
    case ScreenScraperBoxArt::Box2DBack:
        return metadata.box_2d_back_path;
    case ScreenScraperBoxArt::Support2D:
        return metadata.support_2d_path;
    default:
        return {};
    }
}

std::string SelectedBoxArtPath(const GameMetadata& metadata) {
    return BoxArtPath(metadata, UWP::values.screenscraper_box_art.GetValue());
}

std::string SelectedLargeBoxArtPath(const GameMetadata& metadata) {
    return BoxArtPath(metadata, UWP::values.screenscraper_large_box_art.GetValue());
}

std::string SelectedBackgroundPath(const GameMetadata& metadata) {
    switch (UWP::values.screenscraper_background.GetValue()) {
    case ScreenScraperBackground::FanArt:
        return metadata.fanart_path;
    case ScreenScraperBackground::Screenshot:
        return metadata.screenshot_path;
    case ScreenScraperBackground::Video:
        return metadata.video_path;
    case ScreenScraperBackground::MixRecalboxV1:
        return metadata.mix_v1_path;
    case ScreenScraperBackground::MixRecalboxV2:
        return metadata.mix_v2_path;
    default:
        return {};
    }
}

} // namespace UWP::ScreenScraper
