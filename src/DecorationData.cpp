#include "DecorationData.h"
#include "HttpClient.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace PlotTwist {

std::vector<Decoration>             DecorationData::s_decorations;
std::unordered_map<uint32_t,size_t> DecorationData::s_idIndex;
std::mutex                          DecorationData::s_mutex;
std::atomic<bool>                   DecorationData::s_apiLoaded{false};
std::atomic<bool>                   DecorationData::s_running{false};
AddonAPI_t*                         DecorationData::s_api = nullptr;
std::string                         DecorationData::s_dataDir;
std::function<void()>               DecorationData::s_onApiLoaded;
std::function<void()>               DecorationData::s_onMetaLoaded;
std::thread                         DecorationData::s_thread;

void DecorationData::Initialize(AddonAPI_t* api, const std::string& dataDir) {
    s_api     = api;
    s_dataDir = dataDir;
    s_running = true;
    s_thread  = std::thread(FetchThread);
}

void DecorationData::Shutdown() {
    s_running = false;
    if (s_thread.joinable()) s_thread.join();
}

std::vector<Decoration> DecorationData::GetDecorations() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_decorations;
}

const Decoration* DecorationData::FindById(uint32_t id) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_idIndex.find(id);
    if (it == s_idIndex.end()) return nullptr;
    return &s_decorations[it->second];
}

std::optional<Decoration> DecorationData::FindByIdCopy(uint32_t id) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_idIndex.find(id);
    if (it == s_idIndex.end()) return std::nullopt;
    return s_decorations[it->second]; // returns a copy while mutex is held
}

const Decoration* DecorationData::FindByName(const std::string& name) {
    std::lock_guard<std::mutex> lock(s_mutex);
    for (auto& d : s_decorations) {
        if (d.name == name) return &d;
    }
    return nullptr;
}

bool DecorationData::IsApiLoaded() { return s_apiLoaded; }

void DecorationData::SetOnApiLoaded(std::function<void()> cb) {
    // Must be called before Initialize() to avoid race conditions.
    s_onApiLoaded = std::move(cb);
}

void DecorationData::SetOnMetaLoaded(std::function<void()> cb) {
    // Must be called before Initialize() to avoid race conditions.
    s_onMetaLoaded = std::move(cb);
}

void DecorationData::MergeMetadata(
    const std::unordered_map<uint32_t,
        std::tuple<std::string,std::string,std::string,std::string>>& meta)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    for (auto& d : s_decorations) {
        auto it = meta.find(d.id);
        if (it == meta.end()) continue;
        d.category       = std::get<0>(it->second);
        d.handiworkLevel = std::get<1>(it->second);
        d.expansion      = std::get<2>(it->second);
        d.wikiSlug       = std::get<3>(it->second);
    }
    if (s_onMetaLoaded) s_onMetaLoaded();
}

void DecorationData::LoadApiCache() {
    std::string path = s_dataDir + "/decorations_api.json";
    std::ifstream f(path);
    if (!f.is_open()) return;
    try {
        auto j = nlohmann::json::parse(f);
        std::vector<Decoration> loaded;
        for (auto& item : j) {
            Decoration d;
            d.id      = item["id"].get<uint32_t>();
            d.name    = item.value("name", "");
            d.iconUrl = item.value("iconUrl", "");
            loaded.push_back(std::move(d));
        }
        std::lock_guard<std::mutex> lock(s_mutex);
        s_decorations = std::move(loaded);
        s_idIndex.clear();
        for (size_t i = 0; i < s_decorations.size(); i++)
            s_idIndex[s_decorations[i].id] = i;
        s_apiLoaded = true; // Set inside mutex to guarantee visibility with data
    } catch (...) {}
}

void DecorationData::SaveApiCache() {
    std::string path = s_dataDir + "/decorations_api.json";
    nlohmann::json j = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto& d : s_decorations) {
            j.push_back({{"id", d.id}, {"name", d.name}, {"iconUrl", d.iconUrl}});
        }
    }
    std::ofstream f(path);
    if (f.is_open()) f << j.dump(2);
}

void DecorationData::FetchThread() {
    LoadApiCache(); // serve stale data immediately while fetching

    try {
        auto idsResp = HttpClient::Get(
            "https://api.guildwars2.com/v2/homestead/decorations");
        if (idsResp.empty() || !s_running) return;

        auto allIds = nlohmann::json::parse(idsResp);
        std::vector<Decoration> fetched;

        for (size_t i = 0; i < allIds.size() && s_running; i += 200) {
            std::string idList;
            size_t end = std::min(allIds.size(), i + 200);
            for (size_t j = i; j < end; j++) {
                if (j > i) idList += ",";
                idList += std::to_string(allIds[j].get<uint32_t>());
            }
            auto batchResp = HttpClient::Get(
                "https://api.guildwars2.com/v2/homestead/decorations?ids=" + idList);
            if (batchResp.empty()) continue;

            auto items = nlohmann::json::parse(batchResp);
            for (auto& item : items) {
                Decoration d;
                d.id      = item["id"].get<uint32_t>();
                d.name    = item.value("name", "");
                d.iconUrl = item.value("icon", "");
                fetched.push_back(std::move(d));
            }
        }

        if (!s_running || fetched.empty()) return;

        std::sort(fetched.begin(), fetched.end(),
            [](const Decoration& a, const Decoration& b) { return a.name < b.name; });

        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_decorations = std::move(fetched);
            s_idIndex.clear();
            for (size_t i = 0; i < s_decorations.size(); i++)
                s_idIndex[s_decorations[i].id] = i;
            s_apiLoaded = true;
        }
        SaveApiCache();
        if (s_onApiLoaded) s_onApiLoaded();

    } catch (...) {}
}

} // namespace PlotTwist
