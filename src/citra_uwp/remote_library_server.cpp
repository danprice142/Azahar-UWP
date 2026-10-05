// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.

#include "citra_uwp/remote_library_server.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <fmt/format.h>
#include <json.hpp>

#include "common/logging/log.h"

namespace UWP {
namespace {

using boost::asio::ip::tcp;
using json = nlohmann::json;

constexpr std::size_t REQUEST_LIMIT = 4096;
constexpr std::size_t FILE_CHUNK = 64 * 1024;
constexpr std::string_view PLATFORM_NAME = "Nintendo - Nintendo 3DS";

enum class MediaType : unsigned {
    Marquee = 0,
    Cover = 1,
    Box3D = 2,
    MixImage = 3,
    BackCover = 4,
    PhysicalMedia = 5,
    Screenshot = 6,
    FanArt = 7,
    TitleScreen = 8,
    Video = 9,
};

bool IsRegularFile(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(std::filesystem::path{path}, error) && !error;
}

u32 BuildMediaMask(const RemoteGameListEntry& entry) {
    if (!entry.scraped) {
        return 0;
    }
    const auto& media = *entry.scraped;
    u32 mask = 0;
    const auto present = [&mask](MediaType type, const std::string& path) {
        if (IsRegularFile(path)) {
            mask |= 1U << static_cast<unsigned>(type);
        }
    };
    present(MediaType::Cover, media.box_2d_path);
    present(MediaType::Box3D, media.box_3d_path);
    present(MediaType::MixImage,
            !media.mix_v2_path.empty() ? media.mix_v2_path : media.mix_v1_path);
    present(MediaType::Screenshot, media.screenshot_path);
    present(MediaType::FanArt, media.fanart_path);
    present(MediaType::Video, media.video_path);
    return mask;
}

std::string GetContentStem(const RemoteGameListEntry& entry) {
    std::filesystem::path path{entry.path};
    std::string name = path.stem().string();
    if (name.empty()) {
        name = entry.title;
    }
    return name;
}

std::string NormalizeRating(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    char* end = nullptr;
    float score = std::strtof(value.c_str(), &end);
    if (end == value.c_str()) {
        return {};
    }
    // ScreenScraper scores are out of 20; a cached score may already be reduced to 5.
    const float fraction = std::clamp(score > 5.0f ? score / 20.0f : score / 5.0f, 0.0f, 1.0f);
    return fmt::format("{:.3f}", fraction);
}

json BuildCatalogue(const RemoteGameListSnapshot& snapshot) {
    json entries = json::array();
    for (const auto& entry : snapshot.entries) {
        json item{{"label", !entry.title.empty() ? entry.title : entry.long_title},
                  {"path", fmt::format("azahar://{:08X}/{:016X}", snapshot.revision, entry.token)},
                  {"file", GetContentStem(entry)},
                  {"core", "Azahar"},
                  {"core_path", ""},
                  {"db", PLATFORM_NAME},
                  {"crc32", ""},
                  {"img", ""},
                  {"img_full", ""},
                  {"img_short", ""},
                  {"runtime", std::to_string(entry.play_time_seconds)},
                  {"last_played", ""},
                  {"app", "Azahar"},
                  {"launch_revision", snapshot.revision},
                  {"launch_token", fmt::format("{:016X}", entry.token)},
                  {"media", BuildMediaMask(entry)},
                  {"region", entry.region},
                  {"format", entry.file_type},
                  {"compatibility", entry.compatibility},
                  {"size", entry.size_bytes},
                  {"fav", entry.favorite}};

        if (entry.scraped) {
            const auto& metadata = *entry.scraped;
            if (!metadata.synopsis.empty())
                item["desc"] = metadata.synopsis;
            if (!metadata.genre.empty())
                item["genre"] = metadata.genre;
            if (!metadata.developer.empty())
                item["dev"] = metadata.developer;
            if (!metadata.publisher.empty())
                item["pub"] = metadata.publisher;
            if (!metadata.players.empty())
                item["players"] = metadata.players;
            if (!metadata.release_date.empty())
                item["released"] = metadata.release_date;
            if (const std::string rating = NormalizeRating(metadata.rating); !rating.empty())
                item["rating"] = rating;
        }
        entries.emplace_back(std::move(item));
    }

    return json{{"version", 1},
                {"application", "Azahar"},
                {"playlists", json::array({json{{"name", PLATFORM_NAME},
                                                {"count", entries.size()},
                                                {"entries", std::move(entries)}}})}};
}

std::string UrlDecode(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            unsigned decoded = 0;
            const std::string hex{value.substr(index + 1, 2)};
            const auto conversion =
                std::from_chars(hex.data(), hex.data() + hex.size(), decoded, 16);
            if (conversion.ec == std::errc{}) {
                result.push_back(static_cast<char>(decoded));
                index += 2;
                continue;
            }
        }
        result.push_back(value[index] == '+' ? ' ' : value[index]);
    }
    return result;
}

std::string QueryValue(std::string_view query, std::string_view key) {
    while (!query.empty()) {
        const std::size_t separator = query.find('&');
        const std::string_view field = query.substr(0, separator);
        const std::size_t equals = field.find('=');
        if (equals != std::string_view::npos && field.substr(0, equals) == key) {
            return UrlDecode(field.substr(equals + 1));
        }
        if (separator == std::string_view::npos) {
            break;
        }
        query.remove_prefix(separator + 1);
    }
    return {};
}

std::optional<u64> ParseHexToken(std::string_view value) {
    u64 token = 0;
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), token, 16);
    if (value.empty() || conversion.ec != std::errc{} ||
        conversion.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return token;
}

const RemoteGameListEntry* FindEntry(const RemoteGameListSnapshot& snapshot,
                                     std::string_view query) {
    unsigned revision = 0;
    const std::string revision_text = QueryValue(query, "revision");
    const auto revision_conversion = std::from_chars(
        revision_text.data(), revision_text.data() + revision_text.size(), revision);
    const auto token = ParseHexToken(QueryValue(query, "token"));
    // Tokens are stable across revisions, so a frontend holding an older catalogue still resolves.
    if (revision_text.empty() || revision_conversion.ec != std::errc{} ||
        revision_conversion.ptr != revision_text.data() + revision_text.size() || !token) {
        return nullptr;
    }
    const auto entry =
        std::find_if(snapshot.entries.begin(), snapshot.entries.end(),
                     [token](const auto& candidate) { return candidate.token == *token; });
    return entry == snapshot.entries.end() ? nullptr : &*entry;
}

std::string MediaPath(const RemoteGameListEntry& entry, unsigned type) {
    if (!entry.scraped) {
        return {};
    }
    const auto& media = *entry.scraped;
    switch (static_cast<MediaType>(type)) {
    case MediaType::Cover:
        return media.box_2d_path;
    case MediaType::Box3D:
        return media.box_3d_path;
    case MediaType::MixImage:
        return !media.mix_v2_path.empty() ? media.mix_v2_path : media.mix_v1_path;
    case MediaType::Screenshot:
        return media.screenshot_path;
    case MediaType::FanArt:
        return media.fanart_path;
    case MediaType::Video:
        return media.video_path;
    default:
        return {};
    }
}

std::string ContentType(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension == ".png")
        return "image/png";
    if (extension == ".jpg" || extension == ".jpeg")
        return "image/jpeg";
    if (extension == ".webp")
        return "image/webp";
    if (extension == ".mp4")
        return "video/mp4";
    if (extension == ".mkv")
        return "video/x-matroska";
    if (extension == ".webm")
        return "video/webm";
    return "application/octet-stream";
}

bool SendAll(tcp::socket& socket, const void* data, std::size_t size) {
    boost::system::error_code error;
    boost::asio::write(socket, boost::asio::buffer(data, size), error);
    return !error;
}

void SendHeader(tcp::socket& socket, std::string_view status, std::string_view content_type,
                std::uintmax_t length) {
    const std::string header =
        fmt::format("HTTP/1.1 {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n"
                    "Access-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\n"
                    "Connection: close\r\n\r\n",
                    status, content_type, length);
    SendAll(socket, header.data(), header.size());
}

void SendText(tcp::socket& socket, std::string_view status, std::string_view content_type,
              std::string_view body) {
    SendHeader(socket, status, content_type, body.size());
    if (!body.empty()) {
        SendAll(socket, body.data(), body.size());
    }
}

void SendFile(tcp::socket& socket, const std::string& path) {
    std::error_code error;
    const auto length = std::filesystem::file_size(path, error);
    if (error) {
        SendText(socket, "404 Not Found", "text/plain", "404 Not Found");
        return;
    }
    std::ifstream input{std::filesystem::path{path}, std::ios::binary};
    if (!input) {
        SendText(socket, "404 Not Found", "text/plain", "404 Not Found");
        return;
    }
    SendHeader(socket, "200 OK", ContentType(path), length);
    std::vector<char> chunk(FILE_CHUNK);
    while (input) {
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize count = input.gcount();
        if (count <= 0 || !SendAll(socket, chunk.data(), static_cast<std::size_t>(count))) {
            break;
        }
    }
}

} // namespace

struct RemoteLibraryServer::Impl {
    bool Start(u16 requested_port, SnapshotProvider requested_provider) {
        Stop();
        port = requested_port;
        provider = std::move(requested_provider);
        startup_finished = false;
        requested = true;
        worker = std::thread([this] { Worker(); });

        std::unique_lock lock{startup_mutex};
        startup_condition.wait_for(lock, std::chrono::seconds{3},
                                   [this] { return startup_finished; });
        return listening.load(std::memory_order_acquire);
    }

    void Stop() {
        requested.store(false, std::memory_order_release);
        if (worker.joinable()) {
            worker.join();
        }
        listening.store(false, std::memory_order_release);
        acceptor.reset();
        provider = {};
    }

    void Worker() {
        boost::system::error_code error;
        acceptor = std::make_unique<tcp::acceptor>(context);
        acceptor->open(tcp::v4(), error);
        if (!error)
            acceptor->set_option(tcp::acceptor::reuse_address{true}, error);
        if (!error)
            acceptor->bind(tcp::endpoint{tcp::v4(), port}, error);
        if (!error)
            acceptor->listen(4, error);
        if (!error)
            acceptor->non_blocking(true, error);

        {
            std::lock_guard lock{startup_mutex};
            startup_finished = true;
            listening.store(!error, std::memory_order_release);
        }
        startup_condition.notify_all();

        if (error) {
            LOG_ERROR(Frontend, "Remote library server could not listen on port {}: {}", port,
                      error.message());
            requested.store(false, std::memory_order_release);
            return;
        }
        LOG_INFO(Frontend, "Remote library server listening on port {}", port);

        while (requested.load(std::memory_order_acquire)) {
            tcp::socket client{context};
            acceptor->accept(client, error);
            if (error == boost::asio::error::would_block ||
                error == boost::asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
                continue;
            }
            if (error) {
                LOG_WARNING(Frontend, "Remote library server accept failed: {}", error.message());
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
                continue;
            }
            HandleClient(client);
        }

        boost::system::error_code ignored;
        acceptor->close(ignored);
        listening.store(false, std::memory_order_release);
        LOG_INFO(Frontend, "Remote library server stopped");
    }

    void HandleClient(tcp::socket& client) {
        std::array<char, REQUEST_LIMIT> request{};
        boost::system::error_code error;
        const std::size_t received = client.read_some(boost::asio::buffer(request), error);
        if (error || received == 0) {
            return;
        }
        const std::string_view text{request.data(), received};
        const std::size_t line_end = text.find("\r\n");
        const std::string_view line = text.substr(0, line_end);
        if (!line.starts_with("GET ")) {
            SendText(client, "405 Method Not Allowed", "text/plain", "405 Method Not Allowed");
            return;
        }
        const std::size_t target_end = line.find(' ', 4);
        if (target_end == std::string_view::npos) {
            SendText(client, "400 Bad Request", "text/plain", "400 Bad Request");
            return;
        }
        const std::string_view target = line.substr(4, target_end - 4);
        const std::size_t question = target.find('?');
        const std::string_view path = target.substr(0, question);
        const std::string_view query =
            question == std::string_view::npos ? std::string_view{} : target.substr(question + 1);

        if (path == "/catalogue") {
            const std::string body = BuildCatalogue(provider()).dump();
            SendText(client, "200 OK", "application/json", body);
            return;
        }
        if (!path.starts_with("/media/")) {
            SendText(client, "404 Not Found", "text/plain", "404 Not Found");
            return;
        }

        unsigned media_type = 0;
        const std::string_view type_text = path.substr(7);
        const auto type_conversion =
            std::from_chars(type_text.data(), type_text.data() + type_text.size(), media_type);
        if (type_text.empty() || type_conversion.ec != std::errc{} ||
            type_conversion.ptr != type_text.data() + type_text.size() || media_type > 9) {
            SendText(client, "404 Not Found", "text/plain", "404 Not Found");
            return;
        }

        const RemoteGameListSnapshot snapshot = provider();
        const RemoteGameListEntry* entry = FindEntry(snapshot, query);
        const std::string media_path = entry ? MediaPath(*entry, media_type) : std::string{};
        if (!IsRegularFile(media_path)) {
            SendText(client, "404 Not Found", "text/plain", "404 Not Found");
            return;
        }
        SendFile(client, media_path);
    }

    boost::asio::io_context context;
    std::unique_ptr<tcp::acceptor> acceptor;
    SnapshotProvider provider;
    std::thread worker;
    std::atomic_bool requested{false};
    std::atomic_bool listening{false};
    u16 port = 0;
    std::mutex startup_mutex;
    std::condition_variable startup_condition;
    bool startup_finished = false;
};

RemoteLibraryServer::RemoteLibraryServer() : impl{std::make_unique<Impl>()} {}

RemoteLibraryServer::~RemoteLibraryServer() {
    Stop();
}

bool RemoteLibraryServer::Start(u16 port, SnapshotProvider provider) {
    return impl->Start(port, std::move(provider));
}

void RemoteLibraryServer::Stop() {
    impl->Stop();
}

bool RemoteLibraryServer::IsRunning() const {
    return impl->listening.load(std::memory_order_acquire);
}

} // namespace UWP
