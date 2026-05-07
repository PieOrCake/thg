#include "IconCache.h"
#include "HttpClient.h"
#include <filesystem>
#include <chrono>

namespace TyrianHomeAndGarden {

AddonAPI_t*             IconCache::s_api     = nullptr;
std::string             IconCache::s_dataDir;
std::atomic<bool>       IconCache::s_running{false};
std::thread             IconCache::s_thread;
std::mutex              IconCache::s_queueMutex;
std::condition_variable IconCache::s_cv;
std::vector<IconCache::QueueItem> IconCache::s_queue;
std::mutex              IconCache::s_readyMutex;
std::vector<IconCache::ReadyItem> IconCache::s_ready;
std::mutex              IconCache::s_cacheMutex;
std::unordered_map<uint32_t, Texture_t*> IconCache::s_textures;
std::unordered_map<uint32_t, bool>       IconCache::s_loading;
std::unordered_map<uint32_t, int64_t>    IconCache::s_failTime;
std::mutex              IconCache::s_pendingMutex;
std::unordered_map<std::string, uint32_t> IconCache::s_pendingIds;

static int64_t NowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void IconCache::Initialize(AddonAPI_t* api, const std::string& dataDir) {
    s_api     = api;
    s_dataDir = dataDir;
    std::filesystem::create_directories(s_dataDir + "/icons");
    s_running = true;
    s_thread  = std::thread(WorkerThread);
}

void IconCache::Shutdown() {
    s_running = false;
    s_cv.notify_all();
    if (s_thread.joinable()) s_thread.join();
}

void IconCache::Request(uint32_t id, const std::string& iconUrl) {
    if (iconUrl.empty()) return;
    {
        std::lock_guard<std::mutex> lk(s_cacheMutex);
        if (s_textures.count(id)) return;
        if (s_loading.count(id) && s_loading[id]) return;
        if (s_failTime.count(id) && NowSec() - s_failTime[id] < 600) return;
        s_loading[id] = true;
    }
    std::string path = s_dataDir + "/icons/" + std::to_string(id) + ".png";
    if (std::filesystem::exists(path)) {
        std::lock_guard<std::mutex> rl(s_readyMutex);
        s_ready.push_back({id, path});
        return;
    }
    {
        std::lock_guard<std::mutex> ql(s_queueMutex);
        s_queue.push_back({id, iconUrl});
    }
    s_cv.notify_one();
}

Texture_t* IconCache::GetTexture(uint32_t id) {
    std::lock_guard<std::mutex> lk(s_cacheMutex);
    auto it = s_textures.find(id);
    return it != s_textures.end() ? it->second : nullptr;
}

void IconCache::OnTextureLoaded(const char* identifier, Texture_t* texture) {
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

void IconCache::Tick() {
    std::vector<ReadyItem> batch;
    {
        std::lock_guard<std::mutex> rl(s_readyMutex);
        batch.swap(s_ready);
    }
    for (auto& item : batch) {
        std::string texId = "THG_ICON_" + std::to_string(item.id);
        {
            std::lock_guard<std::mutex> pl(s_pendingMutex);
            s_pendingIds[texId] = item.id;
        }
        s_api->Textures_LoadFromFile(texId.c_str(), item.filePath.c_str(), OnTextureLoaded);
    }
}

void IconCache::WorkerThread() {
    while (s_running) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lk(s_queueMutex);
            s_cv.wait(lk, [&]{ return !s_queue.empty() || !s_running; });
            if (!s_running) break;
            item = s_queue.front();
            s_queue.erase(s_queue.begin());
        }
        std::string path = s_dataDir + "/icons/" + std::to_string(item.id) + ".png";
        bool ok = HttpClient::DownloadToFile(item.iconUrl, path);
        if (ok) {
            std::lock_guard<std::mutex> rl(s_readyMutex);
            s_ready.push_back({item.id, path});
        } else {
            std::lock_guard<std::mutex> lk(s_cacheMutex);
            s_loading[item.id]  = false;
            s_failTime[item.id] = NowSec();
        }
    }
}

} // namespace TyrianHomeAndGarden
