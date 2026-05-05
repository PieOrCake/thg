#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include "nexus/Nexus.h"

namespace PlotTwist {

class WikiPreview {
public:
    static void Initialize(AddonAPI_t* api, const std::string& dataDir);
    static void Shutdown();
    static void Tick(); // call every frame on render thread

    static void      Request(uint32_t id, const std::string& wikiSlug);
    static Texture_t* GetTexture(uint32_t id);
    static bool       IsLoading(uint32_t id);

private:
    static void WorkerThread();
    static std::string FetchImageUrl(const std::string& wikiSlug);
    static bool        DownloadImage(const std::string& url, const std::string& destPath);

    // Callback invoked by Nexus on the render thread when a texture is ready
    static void OnTextureLoaded(const char* identifier, Texture_t* texture);

    struct QueueItem { uint32_t id; std::string wikiSlug; };
    struct ReadyItem { uint32_t id; std::string filePath; };

    static AddonAPI_t*              s_api;
    static std::string              s_dataDir;
    static std::atomic<bool>        s_running;
    static std::thread              s_thread;

    static std::mutex               s_queueMutex;
    static std::condition_variable  s_cv;
    static std::vector<QueueItem>   s_queue;

    static std::mutex               s_readyMutex;
    static std::vector<ReadyItem>   s_ready;

    static std::mutex               s_cacheMutex;
    static std::unordered_map<uint32_t, Texture_t*> s_textures;
    static std::unordered_map<uint32_t, bool>        s_loading;
    static std::unordered_map<uint32_t, int64_t>     s_failTime;

    // Maps texture identifier string back to item id for the callback
    static std::mutex                                    s_pendingMutex;
    static std::unordered_map<std::string, uint32_t>     s_pendingIds;
};

} // namespace PlotTwist
