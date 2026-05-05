#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <functional>
#include <thread>
#include "nexus/Nexus.h"

namespace PlotTwist {

struct Decoration {
    uint32_t    id            = 0;
    std::string name;
    std::string iconUrl;
    std::string category;
    std::string handiworkLevel;
    std::string expansion;
    std::string wikiSlug;
};

class DecorationData {
public:
    static void Initialize(AddonAPI_t* api, const std::string& dataDir);
    static void Shutdown();

    static std::vector<Decoration> GetDecorations();
    static const Decoration*       FindById(uint32_t id);
    static const Decoration*       FindByName(const std::string& name);
    static bool                    IsApiLoaded();

    // Called by MetadataScraper when wiki data is ready.
    // meta: id -> {category, handiworkLevel, expansion, wikiSlug}
    static void MergeMetadata(
        const std::unordered_map<uint32_t,
            std::tuple<std::string,std::string,std::string,std::string>>& meta);

    static void SetOnApiLoaded(std::function<void()> cb);
    static void SetOnMetaLoaded(std::function<void()> cb);

private:
    static void FetchThread();
    static void LoadApiCache();
    static void SaveApiCache();

    static std::vector<Decoration>             s_decorations;
    static std::unordered_map<uint32_t,size_t> s_idIndex;
    static std::mutex                          s_mutex;
    static std::atomic<bool>                   s_apiLoaded;
    static std::atomic<bool>                   s_running;
    static AddonAPI_t*                         s_api;
    static std::string                         s_dataDir;
    static std::function<void()>               s_onApiLoaded;
    static std::function<void()>               s_onMetaLoaded;
    static std::thread                         s_thread;
};

} // namespace PlotTwist
