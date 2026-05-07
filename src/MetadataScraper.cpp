#include "MetadataScraper.h"
#include "DecorationData.h"
#include "HttpClient.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>

namespace TyrianHomeAndGarden {

AddonAPI_t*       MetadataScraper::s_api     = nullptr;
std::string       MetadataScraper::s_dataDir;
std::atomic<bool> MetadataScraper::s_running{false};
std::thread       MetadataScraper::s_thread;

void MetadataScraper::Initialize(AddonAPI_t* api, const std::string& dataDir) {
    s_api     = api;
    s_dataDir = dataDir;
    s_running = true;
    s_thread  = std::thread(WorkerThread);
}

void MetadataScraper::Shutdown() {
    s_running = false;
    if (s_thread.joinable()) s_thread.join();
}

std::string MetadataScraper::LoadRevision() {
    std::ifstream f(s_dataDir + "/meta_revision.txt");
    std::string r; std::getline(f, r); return r;
}

void MetadataScraper::SaveRevision(const std::string& rev) {
    std::ofstream f(s_dataDir + "/meta_revision.txt");
    if (f.is_open()) f << rev;
}

std::string MetadataScraper::FetchLiveRevision() {
    try {
        auto resp = HttpClient::Get(
            "https://wiki.guildwars2.com/api.php"
            "?action=query&titles=Decoration/Homestead"
            "&prop=revisions&rvprop=ids&format=json");
        if (resp.empty()) return {};
        auto j = nlohmann::json::parse(resp);
        for (auto& [key, page] : j["query"]["pages"].items())
            return std::to_string(page["revisions"][0]["revid"].get<int64_t>());
    } catch (...) {}
    return {};
}

std::string MetadataScraper::CheckNeedsUpdate() {
    std::ifstream cache(s_dataDir + "/decorations_meta.json");
    if (!cache.is_open()) {
        // No cache — scrape is needed; fetch live revision now
        return FetchLiveRevision();
    }
    cache.close();
    std::string stored = LoadRevision();
    // Empty stored revision: re-scrape to establish baseline
    std::string live = FetchLiveRevision();
    if (live.empty()) return {};        // network error — keep existing
    if (!stored.empty() && live == stored) return {};  // up to date
    return live;
}

// ---- HTML parsing helpers (file-scope, not exposed) ----

static bool ClassHasToken(const std::string& classes, const std::string& token) {
    std::string padded = " " + classes + " ";
    return padded.find(" " + token + " ") != std::string::npos;
}

static std::string ClassesToCategory(const std::string& classes) {
    static const std::vector<std::pair<std::string,std::string>> kMap = {
        {"f-architecture",           "Architecture"},
        {"f-table-and-seating",      "Table, Seating, Etc."},
        {"f-storage",                "Storage"},
        {"f-decor",                  "Decor"},
        {"f-lighting",               "Lighting"},
        {"f-planters-and-topiaries", "Planters and Topiaries"},
        {"f-trees-and-foliages",     "Trees and Foliage"},
        {"f-natural-features",       "Natural Features"},
        {"f-sculptures",             "Sculptures"},
        {"f-flag-signs-markers",     "Flag, Signs, Markers, Etc."},
        {"f-weapons-and-traps",      "Weapons and Traps"},
        {"f-trophies",               "Trophies"},
        {"f-racing",                 "Racing"},
        {"f-other",                  "Other"},
        {"f-black-lion",             "Black Lion"},
    };
    for (auto& [cls, name] : kMap)
        if (ClassHasToken(classes, cls)) return name;
    return "Other";
}

static std::string ClassesToExpansion(const std::string& classes) {
    static const std::vector<std::pair<std::string,std::string>> kExp = {
        {"f-heart-of-thorns",          "Heart of Thorns"},
        {"f-path-of-fire",             "Path of Fire"},
        {"f-end-of-dragons",           "End of Dragons"},
        {"f-secrets-of-the-obscure",   "Secrets of the Obscure"},
        {"f-janthir-wilds",            "Janthir Wilds"},
        {"f-visions-of-eternity",      "Visions of Eternity"},
    };
    static const std::vector<std::pair<std::string,std::string>> kFest = {
        {"f-lunar-new-year",             "Lunar New Year"},
        {"f-super-adventure-box",        "Super Adventure Box"},
        {"f-dragon-bash",                "Dragon Bash"},
        {"f-festival-of-the-four-winds", "Festival of the Four Winds"},
        {"f-shadow-of-the-mad-king",     "Shadow of the Mad King"},
        {"f-wintersday",                 "Wintersday"},
    };
    for (auto& [cls, name] : kExp)
        if (ClassHasToken(classes, cls)) return name;
    for (auto& [cls, name] : kFest)
        if (ClassHasToken(classes, cls)) return name;
    return "Core";
}

static std::string ClassesToHandiwork(const std::string& classes) {
    if (ClassHasToken(classes, "f-400")) return "Grandmaster";
    if (ClassHasToken(classes, "f-300")) return "Grandmaster";
    if (ClassHasToken(classes, "f-225")) return "Master";
    if (ClassHasToken(classes, "f-150")) return "Adept";
    if (ClassHasToken(classes, "f-75"))  return "Journeyman";
    if (ClassHasToken(classes, "f-1"))   return "Novice";
    return "";
}

static std::string DecodeHtmlEntities(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '&') {
            auto semi = s.find(';', i + 1);
            if (semi != std::string::npos) {
                std::string entity = s.substr(i, semi - i + 1);
                if (entity == "&amp;")  { result += '&';  i = semi + 1; continue; }
                if (entity == "&lt;")   { result += '<';  i = semi + 1; continue; }
                if (entity == "&gt;")   { result += '>';  i = semi + 1; continue; }
                if (entity == "&quot;") { result += '"';  i = semi + 1; continue; }
                if (entity == "&apos;") { result += '\''; i = semi + 1; continue; }
                // Numeric decimal: &#N;
                if (s[i+1] == '#' && semi > i + 2) {
                    try {
                        int code = std::stoi(s.substr(i + 2, semi - i - 2));
                        if (code > 0 && code < 128) {
                            result += (char)code;
                            i = semi + 1;
                            continue;
                        }
                    } catch (...) {}
                }
            }
        }
        result += s[i++];
    }
    return result;
}

// Extract the decoration name from a filter-plain div's heading.
// The heading has two <a> tags: first wraps <img> (icon), second contains the name.
static std::string ExtractName(const std::string& html, size_t divStart) {
    auto headingPos = html.find("<div class=\"heading\">", divStart);
    if (headingPos == std::string::npos) return {};

    // Skip the first <a>...</a> (icon link, contains <img>)
    auto firstA = html.find("<a ", headingPos);
    if (firstA == std::string::npos) return {};
    auto firstAEnd = html.find("</a>", firstA);
    if (firstAEnd == std::string::npos) return {};

    // Second <a> contains the name
    auto secondA = html.find("<a ", firstAEnd + 4);
    if (secondA == std::string::npos) return {};
    auto nameClose = html.find('>', secondA);
    if (nameClose == std::string::npos) return {};
    auto nameEnd = html.find("</a>", nameClose + 1);
    if (nameEnd == std::string::npos) return {};

    std::string raw = html.substr(nameClose + 1, nameEnd - nameClose - 1);
    return DecodeHtmlEntities(raw);
}

MetaMap MetadataScraper::ScrapeWiki() {
    MetaMap result;

    auto resp = HttpClient::Get(
        "https://wiki.guildwars2.com/api.php"
        "?action=parse&page=Decoration/Homestead&prop=text&format=json");
    if (resp.empty()) return result;

    std::string html;
    try {
        auto j = nlohmann::json::parse(resp);
        html = j["parse"]["text"]["*"].get<std::string>();
    } catch (...) { return result; }

    const std::string kPrefix = "<div class=\"filter-plain ";
    size_t pos = 0;

    while (pos < html.size() && s_running) {
        auto divPos = html.find(kPrefix, pos);
        if (divPos == std::string::npos) break;

        // Extract full class attribute value (from start of class value to closing quote)
        size_t classValStart = divPos + 12;           // skip '<div class="'
        size_t classValEnd   = html.find('"', classValStart);
        if (classValEnd == std::string::npos) { pos = divPos + kPrefix.size(); continue; }
        std::string classes = html.substr(classValStart, classValEnd - classValStart);

        // Extract decoration name
        std::string name = ExtractName(html, divPos);
        if (name.empty()) { pos = divPos + kPrefix.size(); continue; }

        // Match by name against loaded decoration data — use ID copy to avoid holding
        // a raw pointer into s_decorations across a mutex release.
        uint32_t decorId = DecorationData::FindIdByName(name);
        if (decorId == 0) { pos = divPos + kPrefix.size(); continue; }

        // Derive wikiSlug from decoration name (decoration page, not Handiwork recipe page)
        std::string slug = name;
        std::replace(slug.begin(), slug.end(), ' ', '_');

        result[decorId] = {
            ClassesToCategory(classes),
            ClassesToHandiwork(classes),
            ClassesToExpansion(classes),
            slug
        };

        pos = divPos + kPrefix.size();
    }

    return result;
}

MetaMap MetadataScraper::LoadCache() {
    MetaMap result;
    std::ifstream f(s_dataDir + "/decorations_meta.json");
    if (!f.is_open()) return result;
    try {
        auto j = nlohmann::json::parse(f);
        for (auto& [key, val] : j.items()) {
            uint32_t id = std::stoul(key);
            result[id] = {
                val.value("category", ""),
                val.value("handiworkLevel", ""),
                val.value("expansion", ""),
                val.value("wikiSlug", "")
            };
        }
    } catch (...) {}
    return result;
}

void MetadataScraper::SaveCache(const MetaMap& meta) {
    nlohmann::json j;
    for (auto& [id, t] : meta) {
        j[std::to_string(id)] = {
            {"category",       std::get<0>(t)},
            {"handiworkLevel", std::get<1>(t)},
            {"expansion",      std::get<2>(t)},
            {"wikiSlug",       std::get<3>(t)}
        };
    }
    std::ofstream f(s_dataDir + "/decorations_meta.json");
    if (f.is_open()) f << j.dump(2);
}

void MetadataScraper::WorkerThread() {
    auto cached = LoadCache();
    if (!cached.empty()) DecorationData::MergeMetadata(cached);

    std::string liveRev = CheckNeedsUpdate();
    if (liveRev.empty() || !s_running) return;

    // Wait for the GW2 API decoration list to be loaded before scraping.
    // On fresh install both threads race; if ScrapeWiki wins, FindIdByName
    // matches nothing and the entire scrape result is discarded.
    while (s_running && !DecorationData::IsApiLoaded())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!s_running) return;

    auto meta = ScrapeWiki();
    if (meta.empty() || !s_running) return;

    SaveCache(meta);
    SaveRevision(liveRev);
    DecorationData::MergeMetadata(meta);
}

} // namespace TyrianHomeAndGarden
