#pragma once
#include <string>
#include <unordered_map>
#include <tuple>
#include <atomic>
#include <thread>
#include "nexus/Nexus.h"

namespace TyrianHomeAndGarden {

using MetaMap = std::unordered_map<uint32_t,
    std::tuple<std::string,std::string,std::string,std::string>>;
// value: {category, handiworkLevel, expansion, wikiSlug}

class MetadataScraper {
public:
    static void Initialize(AddonAPI_t* api, const std::string& dataDir);
    static void Shutdown();

private:
    static void   WorkerThread();
    static std::string CheckNeedsUpdate(); // returns live revid string if update needed, "" if not
    static MetaMap ScrapeWiki();
    static MetaMap LoadCache();
    static void   SaveCache(const MetaMap& meta);
    static void   SaveRevision(const std::string& rev);
    static std::string LoadRevision();
    static std::string FetchLiveRevision();

    static AddonAPI_t*       s_api;
    static std::string       s_dataDir;
    static std::atomic<bool> s_running;
    static std::thread       s_thread;
};

} // namespace TyrianHomeAndGarden
