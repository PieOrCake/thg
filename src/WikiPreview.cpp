#include "WikiPreview.h"
#include "HttpClient.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <chrono>

namespace TyrianHomeAndGarden {

AddonAPI_t*             WikiPreview::s_api     = nullptr;
std::string             WikiPreview::s_dataDir;
std::atomic<bool>       WikiPreview::s_running{false};
std::thread             WikiPreview::s_thread;
std::mutex              WikiPreview::s_queueMutex;
std::condition_variable WikiPreview::s_cv;
std::vector<WikiPreview::QueueItem> WikiPreview::s_queue;
std::mutex              WikiPreview::s_readyMutex;
std::vector<WikiPreview::ReadyItem> WikiPreview::s_ready;
std::mutex              WikiPreview::s_cacheMutex;
std::unordered_map<uint32_t, Texture_t*> WikiPreview::s_textures;
std::unordered_map<uint32_t, bool>       WikiPreview::s_loading;
std::unordered_map<uint32_t, int64_t>    WikiPreview::s_failTime;
std::mutex              WikiPreview::s_pendingMutex;
std::unordered_map<std::string, uint32_t> WikiPreview::s_pendingIds;

static int64_t NowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void WikiPreview::Initialize(AddonAPI_t* api, const std::string& dataDir) {
    s_api     = api;
    s_dataDir = dataDir;
    std::filesystem::create_directories(s_dataDir + "/wiki_images");
    s_running = true;
    s_thread  = std::thread(WorkerThread);
}

void WikiPreview::Shutdown() {
    s_running = false;
    s_cv.notify_all();
    if (s_thread.joinable()) s_thread.join();
}

void WikiPreview::Request(uint32_t id, const std::string& wikiSlug, const std::string& fallbackIconUrl) {
    if (wikiSlug.empty() && fallbackIconUrl.empty()) return;
    {
        std::lock_guard<std::mutex> lk(s_cacheMutex);
        if (s_textures.count(id)) return; // already loaded
        if (s_loading.count(id) && s_loading[id]) return; // in flight
        // Respect 600s failure cooldown
        if (s_failTime.count(id) && NowSec() - s_failTime[id] < 600) return;
        s_loading[id] = true;
    }
    // Check disk cache first (may be .png or .jpg)
    std::string pathPng = s_dataDir + "/wiki_images/" + std::to_string(id) + ".png";
    std::string pathJpg = s_dataDir + "/wiki_images/" + std::to_string(id) + ".jpg";
    std::string cachedPath;
    if (std::filesystem::exists(pathPng)) cachedPath = pathPng;
    else if (std::filesystem::exists(pathJpg)) cachedPath = pathJpg;
    if (!cachedPath.empty()) {
        std::lock_guard<std::mutex> rl(s_readyMutex);
        s_ready.push_back({id, cachedPath});
        return;
    }
    {
        std::lock_guard<std::mutex> ql(s_queueMutex);
        s_queue.push_back({id, wikiSlug, fallbackIconUrl});
    }
    s_cv.notify_one();
}

Texture_t* WikiPreview::GetTexture(uint32_t id) {
    std::lock_guard<std::mutex> lk(s_cacheMutex);
    auto it = s_textures.find(id);
    return it != s_textures.end() ? it->second : nullptr;
}

bool WikiPreview::IsLoading(uint32_t id) {
    std::lock_guard<std::mutex> lk(s_cacheMutex);
    auto it = s_loading.find(id);
    return it != s_loading.end() && it->second;
}

// Called by Nexus on the render thread when the texture is ready (or nullptr on failure)
void WikiPreview::OnTextureLoaded(const char* identifier, Texture_t* texture) {
    std::string texId(identifier);
    uint32_t id = 0;
    {
        std::lock_guard<std::mutex> pl(s_pendingMutex);
        auto it = s_pendingIds.find(texId);
        if (it == s_pendingIds.end()) return;
        id = it->second;
        s_pendingIds.erase(it);
    }
    std::lock_guard<std::mutex> lk(s_cacheMutex);
    s_loading[id] = false;
    if (texture)
        s_textures[id] = texture;
    else
        s_failTime[id] = NowSec();
}

void WikiPreview::Tick() {
    std::vector<ReadyItem> batch;
    {
        std::lock_guard<std::mutex> rl(s_readyMutex);
        batch.swap(s_ready);
    }
    for (auto& item : batch) {
        std::string texId = "THG_WIKI_" + std::to_string(item.id);
        {
            std::lock_guard<std::mutex> pl(s_pendingMutex);
            s_pendingIds[texId] = item.id;
        }
        // Nexus loads async and calls our callback on the render thread
        s_api->Textures_LoadFromFile(texId.c_str(), item.filePath.c_str(), OnTextureLoaded);
    }
}

static std::string WikiUrlEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ' ') out += '_';
        else out += c;
    }
    return out;
}

std::string WikiPreview::FetchImageUrl(const std::string& wikiSlug) {
    auto resp = HttpClient::Get(
        "https://wiki.guildwars2.com/api.php?action=query&titles=" +
        WikiUrlEncode(wikiSlug) +
        "&prop=pageimages&pithumbsize=256&format=json");
    if (resp.empty()) return {};
    try {
        auto j = nlohmann::json::parse(resp);
        for (auto& [key, page] : j["query"]["pages"].items()) {
            if (page.contains("thumbnail")) {
                std::string src = page["thumbnail"].value("source", "");
                if (!src.empty()) return src;
            }
        }
    } catch (...) {}
    return {};
}

bool WikiPreview::DownloadImage(const std::string& url, const std::string& destPath) {
    return HttpClient::DownloadToFile(url, destPath);
}

void WikiPreview::WorkerThread() {
    while (s_running) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lk(s_queueMutex);
            s_cv.wait(lk, [&]{ return !s_queue.empty() || !s_running; });
            if (!s_running) break;
            item = s_queue.front();
            s_queue.erase(s_queue.begin());
        }

        std::string url;

        // Try wiki page image first (if wikiSlug provided)
        if (!item.wikiSlug.empty())
            url = FetchImageUrl(item.wikiSlug);

        // Fall back to API icon URL if wiki fetch yielded nothing
        if (url.empty() && !item.fallbackIconUrl.empty())
            url = item.fallbackIconUrl;

        // Detect extension from URL (wiki thumbnails are typically .jpg)
        std::string ext = ".png";
        {
            auto dotPos = url.rfind('.');
            if (dotPos != std::string::npos) {
                std::string tail = url.substr(dotPos);
                // Strip any query string
                auto qPos = tail.find('?');
                if (qPos != std::string::npos) tail = tail.substr(0, qPos);
                // Lowercase comparison
                std::string lower = tail;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (lower == ".jpg" || lower == ".jpeg") ext = ".jpg";
            }
        }
        std::string path = s_dataDir + "/wiki_images/" + std::to_string(item.id) + ext;

        bool ok = !url.empty() && DownloadImage(url, path);
        if (ok) {
            std::lock_guard<std::mutex> rl(s_readyMutex);
            s_ready.push_back({item.id, path});
        } else {
            std::lock_guard<std::mutex> lk(s_cacheMutex);
            s_loading[item.id]  = false;
            s_failTime[item.id] = NowSec();
        }

        // Rate limit: 500ms between requests
        if (s_running) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

} // namespace TyrianHomeAndGarden
