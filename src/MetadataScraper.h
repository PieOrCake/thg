#pragma once
#include <string>
#include <unordered_map>
#include <tuple>
#include <atomic>
#include <thread>
#include "nexus/Nexus.h"

namespace PlotTwist {

using MetaMap = std::unordered_map<uint32_t,
    std::tuple<std::string,std::string,std::string,std::string>>;
// value: {category, handiworkLevel, expansion, wikiSlug}

class MetadataScraper {
public:
    static void Initialize(AddonAPI_t* api, const std::string& dataDir);
    static void Shutdown();

private:
    static void   WorkerThread();
    static bool   NeedsUpdate();          // compares stored vs live lastrevid
    static MetaMap ScrapeWiki();
    static MetaMap LoadCache();
    static void   SaveCache(const MetaMap& meta);
    static void   SaveRevision(const std::string& rev);
    static std::string LoadRevision();
    static std::string FetchLiveRevision();

    // Parses one HTML table row; returns false if row should be skipped.
    static bool ParseRow(const std::string& row, std::string& outName,
                         std::string& outSlug, std::string& outHandiwork,
                         std::string& outExpansion);

    static AddonAPI_t*       s_api;
    static std::string       s_dataDir;
    static std::atomic<bool> s_running;
    static std::thread       s_thread;
};

} // namespace PlotTwist
