#include "RecipeData.h"
#include "DecorationData.h"
#include "HttpClient.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <chrono>

namespace TyrianHomeAndGarden {

AddonAPI_t*             RecipeData::s_api     = nullptr;
std::string             RecipeData::s_dataDir;
std::atomic<bool>       RecipeData::s_running{false};
std::thread             RecipeData::s_thread;
std::mutex              RecipeData::s_queueMutex;
std::condition_variable RecipeData::s_cv;
std::vector<RecipeData::QueueItem> RecipeData::s_queue;
std::mutex              RecipeData::s_readyMutex;
std::vector<RecipeData::ReadyItem> RecipeData::s_ready;
std::mutex              RecipeData::s_cacheMutex;
std::unordered_map<uint32_t, RecipeResult> RecipeData::s_results;
std::unordered_map<uint32_t, bool>         RecipeData::s_loading;
std::mutex              RecipeData::s_recipeIdMutex;
std::unordered_map<uint32_t, uint32_t> RecipeData::s_recipeIds;
std::atomic<bool>       RecipeData::s_recipeIdCacheDirty{false};
std::thread             RecipeData::s_preloadThread;
std::atomic<bool>       RecipeData::s_preloaderRunning{false};

std::mutex              RecipeData::s_ingredientMutex;
std::unordered_map<std::string, std::unordered_set<uint32_t>> RecipeData::s_ingredientIndex;
std::unordered_set<uint32_t> RecipeData::s_ingredientIndexedDecoIds;
std::atomic<bool>       RecipeData::s_ingredientIndexDirty{false};
std::atomic<bool>       RecipeData::s_indexBuilding{false};
std::atomic<int>        RecipeData::s_indexBuildDone{0};
std::atomic<int>        RecipeData::s_indexBuildTotal{0};

// Increment when ingredient ID classification logic changes — forces re-fetch of stale caches.
static constexpr int kCacheVersion = 3;

void RecipeData::Initialize(AddonAPI_t* api, const std::string& dataDir) {
    s_api     = api;
    s_dataDir = dataDir;
    std::filesystem::create_directories(s_dataDir + "/recipes");

    // Load lightweight recipe ID cache for immediate badge display
    std::ifstream f(s_dataDir + "/recipes/recipe_ids.json");
    if (f.is_open()) {
        try {
            auto j = nlohmann::json::parse(f);
            std::lock_guard<std::mutex> lk(s_recipeIdMutex);
            for (auto& [key, val] : j.items())
                s_recipeIds[(uint32_t)std::stoul(key)] = val.get<uint32_t>();
        } catch (...) {}
    }

    // Load persisted ingredient index for instant search availability
    {
        std::ifstream fi(s_dataDir + "/recipes/ingredient_index.json");
        if (fi.is_open()) {
            try {
                auto ji = nlohmann::json::parse(fi);
                std::lock_guard<std::mutex> lk(s_ingredientMutex);
                for (auto& [name, ids] : ji.items()) {
                    for (auto id : ids) {
                        uint32_t decoId = id.get<uint32_t>();
                        s_ingredientIndex[name].insert(decoId);
                        s_ingredientIndexedDecoIds.insert(decoId);
                    }
                }
            } catch (...) {}
        }
    }

    // Background scan: index any recipe cache files not yet in the saved index
    s_indexBuilding = true;
    std::thread([dataDir = s_dataDir]() {
        for (auto& entry : std::filesystem::directory_iterator(dataDir + "/recipes")) {
            auto& p = entry.path();
            if (p.extension() != ".json") continue;
            std::string stem = p.stem().string();
            if (stem == "recipe_ids" || stem == "ingredient_index") continue;
            uint32_t decoId = 0;
            try { decoId = (uint32_t)std::stoul(stem); } catch (...) { continue; }
            {
                std::lock_guard<std::mutex> lk(s_ingredientMutex);
                if (s_ingredientIndexedDecoIds.count(decoId)) continue;
            }
            try {
                std::ifstream f(p);
                auto j = nlohmann::json::parse(f);
                // Skip stale caches — PreloaderThread Phase 2 will re-fetch them
                if (j.value("version", 1) < kCacheVersion) continue;
                RecipeResult r;
                for (auto& ing : j["ingredients"]) {
                    Ingredient i;
                    i.name       = ing.value("name", "");
                    i.itemId     = ing.value("item_id", 0u);
                    i.currencyId = ing.value("currency_id", 0u);
                    i.count      = ing.value("count", 0);
                    r.ingredients.push_back(i);
                }
                IndexResult(decoId, r);
            } catch (...) {}
        }
        // s_indexBuilding stays true — PreloaderThread clears it after Phase 2
    }).detach();

    s_running = true;
    s_thread  = std::thread(WorkerThread);
}

void RecipeData::Shutdown() {
    s_preloaderRunning = false;
    if (s_preloadThread.joinable()) s_preloadThread.join();

    s_running = false;
    s_cv.notify_all();
    if (s_thread.joinable()) s_thread.join();

    if (s_recipeIdCacheDirty) SaveRecipeIdCache();
    if (s_ingredientIndexDirty) SaveIngredientIndex();
}

void RecipeData::Request(uint32_t decoId, const std::string& wikiSlug) {
    if (wikiSlug.empty()) return;
    {
        std::lock_guard<std::mutex> lk(s_cacheMutex);
        if (s_results.count(decoId)) return;
        if (s_loading.count(decoId) && s_loading[decoId]) return;
        s_loading[decoId] = true;
    }
    {
        std::lock_guard<std::mutex> ql(s_queueMutex);
        s_queue.push_back({decoId, wikiSlug});
    }
    s_cv.notify_one();
}

const RecipeResult* RecipeData::GetResult(uint32_t decoId) {
    std::lock_guard<std::mutex> lk(s_cacheMutex);
    auto it = s_results.find(decoId);
    return it != s_results.end() ? &it->second : nullptr;
}

void RecipeData::Tick() {
    std::vector<ReadyItem> batch;
    {
        std::lock_guard<std::mutex> rl(s_readyMutex);
        batch.swap(s_ready);
    }
    for (auto& item : batch) {
        uint32_t rid = item.result.recipeId;
        IndexResult(item.decoId, item.result);
        {
            std::lock_guard<std::mutex> lk(s_cacheMutex);
            s_results[item.decoId] = std::move(item.result);
            s_loading[item.decoId] = false;
        }
        // Propagate newly discovered recipe ID to lightweight cache
        if (rid > 0) {
            std::lock_guard<std::mutex> lk(s_recipeIdMutex);
            if (!s_recipeIds.count(item.decoId)) {
                s_recipeIds[item.decoId] = rid;
                s_recipeIdCacheDirty = true;
            }
        }
    }
}

void RecipeData::IndexResult(uint32_t decoId, const RecipeResult& result) {
    std::lock_guard<std::mutex> lk(s_ingredientMutex);
    if (s_ingredientIndexedDecoIds.count(decoId)) return;
    for (auto& ing : result.ingredients) {
        if (ing.name.empty()) continue;
        std::string lower = ing.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        s_ingredientIndex[lower].insert(decoId);
    }
    s_ingredientIndexedDecoIds.insert(decoId);
    s_ingredientIndexDirty = true;
}

void RecipeData::SaveIngredientIndex() {
    std::lock_guard<std::mutex> lk(s_ingredientMutex);
    try {
        nlohmann::json j;
        for (auto& [name, ids] : s_ingredientIndex)
            j[name] = ids;
        std::ofstream f(s_dataDir + "/recipes/ingredient_index.json");
        f << j.dump();
        s_ingredientIndexDirty = false;
    } catch (...) {}
}

bool RecipeData::IsLoadingRecipe(uint32_t decoId) {
    std::lock_guard<std::mutex> lk(s_cacheMutex);
    auto it = s_loading.find(decoId);
    return it != s_loading.end() && it->second;
}

bool RecipeData::IsPreloading() { return s_preloaderRunning.load(); }
bool RecipeData::IsIndexBuilding() { return s_indexBuilding.load(); }
float RecipeData::GetIndexBuildProgress() {
    int total = s_indexBuildTotal.load();
    if (total <= 0) return 0.0f;
    return std::min(1.0f, (float)s_indexBuildDone.load() / (float)total);
}

bool RecipeData::DecoMatchesIngredient(uint32_t decoId, const std::string& lfilter) {
    std::lock_guard<std::mutex> lk(s_ingredientMutex);
    for (auto& [name, ids] : s_ingredientIndex) {
        if (name.find(lfilter) != std::string::npos && ids.count(decoId))
            return true;
    }
    return false;
}

uint32_t RecipeData::GetRecipeId(uint32_t decoId) {
    std::lock_guard<std::mutex> lk(s_recipeIdMutex);
    auto it = s_recipeIds.find(decoId);
    return it != s_recipeIds.end() ? it->second : 0;
}

void RecipeData::SaveRecipeIdCache() {
    nlohmann::json j = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lk(s_recipeIdMutex);
        for (auto& [decoId, recipeId] : s_recipeIds)
            j[std::to_string(decoId)] = recipeId;
    }
    std::ofstream f(s_dataDir + "/recipes/recipe_ids.json");
    if (f.is_open()) f << j.dump();
    s_recipeIdCacheDirty = false;
}

uint32_t RecipeData::ParseRecipeIdFromWikitext(const std::string& wt) {
    // Find {{Recipe block
    auto blockStart = wt.find("{{Recipe");
    if (blockStart == std::string::npos) return 0;
    auto blockEnd = wt.find("}}", blockStart + 8);
    std::string block = wt.substr(blockStart, blockEnd != std::string::npos ? blockEnd - blockStart : std::string::npos);

    // Scan for pipe-delimited "id" parameter: | id = XXXXX
    size_t p = 0;
    while (p < block.size()) {
        auto bar = block.find('|', p);
        if (bar == std::string::npos) break;
        size_t q = bar + 1;
        while (q < block.size() && (block[q] == ' ' || block[q] == '\n' || block[q] == '\r')) q++;
        if (block.compare(q, 2, "id") == 0) {
            size_t r = q + 2;
            while (r < block.size() && block[r] == ' ') r++;
            if (r < block.size() && block[r] == '=') {
                r++;
                while (r < block.size() && block[r] == ' ') r++;
                uint32_t id = 0;
                while (r < block.size() && std::isdigit((unsigned char)block[r]))
                    id = id * 10 + (block[r++] - '0');
                if (id > 0) return id;
            }
        }
        p = bar + 1;
    }
    return 0;
}

void RecipeData::StartPreloader() {
    if (s_preloaderRunning.exchange(true)) return; // already running
    s_preloadThread = std::thread(PreloaderThread);
}

void RecipeData::PreloaderThread() {
    // Phase 2: network-fetch full recipes for any Handiwork decoration not yet indexed.
    // s_indexBuilding was set in Initialize() and is cleared here when done.
    auto runIndexPhase2 = []() {
        auto decos = DecorationData::GetDecorations();

        // Count work upfront so the progress bar has a denominator immediately
        int total = 0;
        for (auto& d : decos) {
            if (d.handiworkLevel.empty() || d.wikiSlug.empty()) continue;
            std::lock_guard<std::mutex> lk(s_ingredientMutex);
            if (!s_ingredientIndexedDecoIds.count(d.id)) ++total;
        }
        s_indexBuildTotal = total;
        s_indexBuildDone  = 0;

        for (auto& d : decos) {
            if (!s_running) break;
            if (d.handiworkLevel.empty() || d.wikiSlug.empty()) continue;
            {
                std::lock_guard<std::mutex> lk(s_ingredientMutex);
                if (s_ingredientIndexedDecoIds.count(d.id)) continue;
            }
            RecipeResult r;
            if (FetchAndCache(d.id, d.wikiSlug, r))
                IndexResult(d.id, r);
            ++s_indexBuildDone;
            if (s_running)
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        if (s_ingredientIndexDirty) SaveIngredientIndex();
        s_indexBuilding = false;
    };

    // Collect all Handiwork decorations that don't yet have a cached recipe ID
    auto decos = DecorationData::GetDecorations();

    struct Entry { uint32_t decoId; std::string pageTitle; };
    std::vector<Entry> toFetch;
    {
        std::lock_guard<std::mutex> lk(s_recipeIdMutex);
        for (auto& d : decos) {
            if (d.handiworkLevel.empty() || d.wikiSlug.empty()) continue;
            if (s_recipeIds.count(d.id)) continue; // already cached
            toFetch.push_back({d.id, d.wikiSlug + "_(Handiwork)"});
        }
    }

    if (toFetch.empty()) {
        s_preloaderRunning = false;
        runIndexPhase2();
        return;
    }

    // Batch fetch wikitext (50 pages per call — MediaWiki API limit)
    const size_t kBatch = 50;
    bool anySaved = false;

    for (size_t i = 0; i < toFetch.size() && s_preloaderRunning; i += kBatch) {
        size_t end = std::min(i + kBatch, toFetch.size());

        // Build titles string and slug→decoId map
        // MediaWiki normalises underscores to spaces in response titles
        std::unordered_map<std::string, uint32_t> normalizedToDecoId;
        std::string titles;
        for (size_t k = i; k < end; k++) {
            if (k > i) titles += '|';
            titles += toFetch[k].pageTitle;
            std::string norm = toFetch[k].pageTitle;
            for (char& c : norm) if (c == '_') c = ' ';
            normalizedToDecoId[norm] = toFetch[k].decoId;
        }

        auto resp = HttpClient::Get(
            "https://wiki.guildwars2.com/api.php?action=query&prop=revisions&rvprop=content&titles="
            + titles + "&format=json");

        if (!resp.empty()) {
            try {
                auto j = nlohmann::json::parse(resp);
                for (auto& [pageId, page] : j["query"]["pages"].items()) {
                    if (page.contains("missing") || !page.contains("revisions")) continue;
                    std::string title = page["title"].get<std::string>();
                    auto it = normalizedToDecoId.find(title);
                    if (it == normalizedToDecoId.end()) continue;

                    // Try both legacy (*) and slot-based wikitext format
                    std::string wikitext;
                    auto& rev = page["revisions"][0];
                    if (rev.contains("*"))
                        wikitext = rev["*"].get<std::string>();
                    else if (rev.contains("slots"))
                        wikitext = rev["slots"]["main"]["*"].get<std::string>();
                    if (wikitext.empty()) continue;

                    uint32_t recipeId = ParseRecipeIdFromWikitext(wikitext);
                    if (recipeId > 0) {
                        std::lock_guard<std::mutex> lk(s_recipeIdMutex);
                        s_recipeIds[it->second] = recipeId;
                        anySaved = true;
                    }
                }
            } catch (...) {}
        }

        // Save after each batch so partial results survive addon reload
        if (anySaved) {
            SaveRecipeIdCache();
            anySaved = false;
        }

        // Polite rate limit between batches
        if (s_preloaderRunning && (i + kBatch) < toFetch.size())
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    s_preloaderRunning = false;
    runIndexPhase2();
}

// Decode common HTML entities in a string.
static std::string DecodeHtmlEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '&') {
            if (s.compare(i, 5, "&amp;") == 0)  { out += '&'; i += 5; }
            else if (s.compare(i, 4, "&lt;") == 0)   { out += '<'; i += 4; }
            else if (s.compare(i, 4, "&gt;") == 0)   { out += '>'; i += 4; }
            else if (s.compare(i, 6, "&quot;") == 0) { out += '"'; i += 6; }
            else if (s.compare(i, 6, "&apos;") == 0) { out += '\''; i += 6; }
            else if (s.compare(i, 5, "&#39;") == 0)  { out += '\''; i += 5; }
            else { out += s[i++]; }
        } else {
            out += s[i++];
        }
    }
    return out;
}

// Returns the GW2 wallet currency ID for a known currency ingredient name, or 0 if not a currency.
// Built from Crafty Legend's embedded GW2 API currency list and alias map.
static uint32_t GetCurrencyIdByName(const std::string& name) {
    static const std::unordered_map<std::string, uint32_t> kMap = {
        {"Coin", 1},       {"Coins", 1},
        {"Karma", 2},
        {"Laurel", 3},     {"Laurels", 3},
        {"Gem", 4},        {"Gems", 4},
        {"Ascalonian Tear", 5},          {"Ascalonian Tears", 5},
        {"Shard of Zhaitan", 6},         {"Shards of Zhaitan", 6},
        {"Fractal Relic", 7},            {"Fractal Relics", 7},
        {"Seal of Beetletun", 9},
        {"Manifesto of the Moletariate", 10},
        {"Deadly Bloom", 11},
        {"Symbol of Koda", 12},
        {"Flame Legion Charr Carving", 13},
        {"Knowledge Crystal", 14},
        {"Badge of Honor", 15},          {"Badges of Honor", 15},
        {"Guild Commendation", 16},      {"Guild Commendations", 16},
        {"Transmutation Charge", 18},    {"Transmutation Charges", 18},
        {"Airship Part", 19},            {"Airship Parts", 19},
        {"Ley Line Crystal", 20},        {"Ley Line Crystals", 20},
        {"Lump of Aurillium", 22},       {"Lumps of Aurillium", 22},
        {"Spirit Shard", 23},            {"Spirit Shards", 23},
        {"Pristine Fractal Relic", 24},  {"Pristine Fractal Relics", 24},
        {"Geode", 25},                   {"Geodes", 25},
        {"WvW Skirmish Claim Ticket", 26}, {"WvW Skirmish Claim Tickets", 26},
        {"Skirmish Claim Ticket", 26},   {"Skirmish Claim Tickets", 26},
        {"Bandit Crest", 27},            {"Bandit Crests", 27},
        {"Magnetite Shard", 28},         {"Magnetite Shards", 28},
        {"Provisioner Token", 29},       {"Provisioner Tokens", 29},
        {"PvP League Ticket", 30},       {"PvP League Tickets", 30},
        {"League Ticket", 30},           {"League Tickets", 30},
        {"Proof of Heroics", 31},
        {"Unbound Magic", 32},
        {"Ascended Shards of Glory", 33}, {"Ascended Shard of Glory", 33},
        {"Shard of Glory", 33},          {"Shards of Glory", 33},
        {"Trade Contract", 34},          {"Trade Contracts", 34},
        {"Elegy Mosaic", 35},            {"Elegy Mosaics", 35},
        {"Testimony of Desert Heroics", 36},
        {"Exalted Key", 37},
        {"Machete", 38},
        {"Gaeting Crystal", 39},
        {"Bandit Skeleton Key", 40},
        {"Pact Crowbar", 41},
        {"Vial of Chak Acid", 42},
        {"Zephyrite Lockpick", 43},
        {"Trader's Key", 44},
        {"Volatile Magic", 45},
        {"War Supplies", 58},
        {"Unstable Fractal Essence", 59}, {"Unstable Fractal Essences", 59},
        {"Tyrian Defense Seal", 60},
        {"Research Note", 61},           {"Research Notes", 61},
        {"Unusual Coin", 62},            {"Unusual Coins", 62},
        {"Astral Acclaim", 63},
        {"Jade Sliver", 64},             {"Jade Slivers", 64},
        {"Testimony of Jade Heroics", 65},
        {"Ancient Coin", 66},            {"Ancient Coins", 66},
        {"Canach Coins", 67},
        {"Imperial Favor", 68},
        {"Tales of Dungeon Delving", 69},
        {"Legendary Insight", 70},       {"Legendary Insights", 70},
        {"Jade Miner's Keycard", 71},
        {"Static Charge", 72},           {"Static Charges", 72},
        {"Pinch of Stardust", 73},       {"Pinches of Stardust", 73},
        {"Calcified Gasp", 75},          {"Calcified Gasps", 75},
        {"Ursus Oblige", 76},
        {"Fine Rift Essence", 78},       {"Fine Rift Essences", 78},
        {"Rare Rift Essence", 79},       {"Rare Rift Essences", 79},
        {"Masterwork Rift Essence", 80}, {"Masterwork Rift Essences", 80},
        {"Antiquated Ducat", 81},        {"Antiquated Ducats", 81},
        {"Testimony of Castoran Heroics", 82},
        {"Aether-Rich Sap", 83},         {"Aether-Rich Saps", 83},
    };
    auto it = kMap.find(name);
    return it != kMap.end() ? it->second : 0;
}

bool RecipeData::FetchAndCache(uint32_t decoId, const std::string& wikiSlug, RecipeResult& out) {
    std::string cachePath = s_dataDir + "/recipes/" + std::to_string(decoId) + ".json";

    // Try disk cache first
    if (std::filesystem::exists(cachePath)) {
        bool cacheOk = false;
        try {
            std::ifstream f(cachePath);
            auto j = nlohmann::json::parse(f);
            // Reject cache written by an older version of the classification logic
            if (j.value("version", 1) >= kCacheVersion) {
                out.recipeId = j.value("recipe_id", 0u);
                for (auto& ing : j["ingredients"]) {
                    Ingredient i;
                    i.itemId     = ing.value("item_id",     0u);
                    i.currencyId = ing.value("currency_id", 0u);
                    i.count      = ing.value("count",        0);
                    i.name       = ing.value("name",        "");
                    out.ingredients.push_back(i);
                }
                // Also reject if any named ingredient still has no resolved ID
                cacheOk = std::all_of(out.ingredients.begin(), out.ingredients.end(),
                    [](const Ingredient& i){ return i.name.empty() || i.itemId != 0 || i.currencyId != 0; });
            }
            if (!cacheOk) { out.ingredients.clear(); out.recipeId = 0; }
        } catch (...) {}
        if (cacheOk) return true;
    }

    // Fetch the Handiwork recipe page from the GW2 wiki
    std::string pageTitle = wikiSlug + "_(Handiwork)";
    auto resp = HttpClient::Get(
        "https://wiki.guildwars2.com/api.php?action=parse&page=" + pageTitle + "&prop=text&format=json");
    if (resp.empty()) return false;

    std::string html;
    try {
        auto j = nlohmann::json::parse(resp);
        if (!j.contains("parse")) return false; // page missing or redirect fails
        html = j["parse"]["text"]["*"].get<std::string>();
    } catch (...) { return false; }

    // Parse recipe ID from: data-type="recipe" data-id="XXXXX"
    out.recipeId = 0;
    {
        static const std::string kNeedle = "data-type=\"recipe\" data-id=\"";
        auto pos = html.find(kNeedle);
        if (pos != std::string::npos) {
            pos += kNeedle.size();
            auto end = html.find('"', pos);
            if (end != std::string::npos) {
                try { out.recipeId = (uint32_t)std::stoul(html.substr(pos, end - pos)); } catch (...) {}
            }
        }
    }

    // Parse ingredients from <div class="ingredients"><dl>...<dt>N</dt><dd>...<a href="/wiki/SLUG" title="NAME">
    struct RawIngredient { int count; std::string slug; std::string name; };
    std::vector<RawIngredient> rawIngreds;
    {
        auto ingStart = html.find("<div class=\"ingredients\">");
        if (ingStart != std::string::npos) {
            auto dlStart = html.find("<dl>", ingStart);
            auto dlEnd   = html.find("</dl>", dlStart);
            if (dlStart != std::string::npos && dlEnd != std::string::npos) {
                std::string dl = html.substr(dlStart, dlEnd - dlStart);
                size_t p = 0;
                while (p < dl.size()) {
                    // <dt>COUNT</dt>
                    auto dtS = dl.find("<dt>", p);
                    if (dtS == std::string::npos) break;
                    auto dtE = dl.find("</dt>", dtS + 4);
                    if (dtE == std::string::npos) break;
                    int count = 0;
                    try { count = std::stoi(dl.substr(dtS + 4, dtE - dtS - 4)); } catch (...) {}

                    // <dd>...<a href="/wiki/SLUG" title="NAME">
                    auto ddS = dl.find("<dd>", dtE);
                    if (ddS == std::string::npos) break;
                    auto ddE = dl.find("</dd>", ddS);

                    static const std::string kHref = "<a href=\"/wiki/";
                    auto aS = dl.find(kHref, ddS);
                    if (aS == std::string::npos || (ddE != std::string::npos && aS > ddE)) {
                        p = (ddE != std::string::npos) ? ddE + 5 : dl.size();
                        continue;
                    }
                    aS += kHref.size();
                    auto aE = dl.find('"', aS);
                    if (aE == std::string::npos) break;
                    std::string slug = dl.substr(aS, aE - aS);

                    // title attribute has the display name
                    static const std::string kTitle = "title=\"";
                    auto tS = dl.find(kTitle, aE);
                    std::string name;
                    if (tS != std::string::npos && (ddE == std::string::npos || tS < ddE)) {
                        tS += kTitle.size();
                        auto tE = dl.find('"', tS);
                        if (tE != std::string::npos)
                            name = DecodeHtmlEntities(dl.substr(tS, tE - tS));
                    }
                    if (name.empty()) {
                        // Fallback: derive name from slug (replace _ with space)
                        name = slug;
                        for (char& c : name) if (c == '_') c = ' ';
                    }

                    if (count > 0 && !slug.empty())
                        rawIngreds.push_back({count, slug, name});
                    p = (ddE != std::string::npos) ? ddE + 5 : dl.size();
                }
            }
        }
    }

    // Batch-fetch ingredient item IDs via wiki extlinks
    // action=query&titles=Slug1|Slug2|...&prop=extlinks returns external links per page.
    // GW2 items show: https://api.guildwars2.com/v2/items?ids=XXXXX&lang=en
    if (!rawIngreds.empty()) {
        std::string titles;
        for (size_t i = 0; i < rawIngreds.size(); i++) {
            if (i) titles += '|';
            titles += rawIngreds[i].slug;
        }
        auto extResp = HttpClient::Get(
            "https://wiki.guildwars2.com/api.php?action=query&titles=" + titles + "&prop=extlinks&format=json");

        // name → itemId / currencyId (normalized page title == ingredient display name)
        // Currency IDs take priority: if a page has both an item and a currency link,
        // the ingredient is a wallet currency and must be queried via EV_HOARD_QUERY_WALLET.
        std::unordered_map<std::string, uint32_t> nameToItemId;
        std::unordered_map<std::string, uint32_t> nameToCurrencyId;
        if (!extResp.empty()) {
            try {
                auto j = nlohmann::json::parse(extResp);
                static const std::string kItemPrefix     = "https://api.guildwars2.com/v2/items?ids=";
                static const std::string kCurrencyPrefix = "https://api.guildwars2.com/v2/currencies?ids=";
                auto parseId = [](const std::string& url, const std::string& prefix) -> uint32_t {
                    if (url.find(prefix) != 0) return 0;
                    size_t start = prefix.size();
                    auto end = url.find_first_of("&?", start);
                    try { return (uint32_t)std::stoul(url.substr(start, end == std::string::npos ? std::string::npos : end - start)); }
                    catch (...) { return 0; }
                };
                for (auto& [pageId, page] : j["query"]["pages"].items()) {
                    if (!page.contains("extlinks")) continue;
                    std::string pageTitle2 = page["title"].get<std::string>();
                    uint32_t foundItem = 0, foundCurrency = 0;
                    for (auto& link : page["extlinks"]) {
                        std::string url = link["*"].get<std::string>();
                        if (!foundItem)     foundItem     = parseId(url, kItemPrefix);
                        if (!foundCurrency) foundCurrency = parseId(url, kCurrencyPrefix);
                        if (foundItem && foundCurrency) break;
                    }
                    // Prefer currency: if a page has both links, it's a wallet currency
                    if (foundCurrency) nameToCurrencyId[pageTitle2] = foundCurrency;
                    else if (foundItem) nameToItemId[pageTitle2]     = foundItem;
                }
            } catch (...) {}
        }

        // Fallback: for any ingredient whose ID wasn't resolved via extlinks,
        // fetch its wiki page and look for data-type="item"/"currency" data-id="XXXXX"
        // in the rendered HTML — the same technique used to extract recipe IDs.
        for (auto& raw : rawIngreds) {
            if (nameToItemId.count(raw.name) || nameToCurrencyId.count(raw.name)) continue;
            auto pageResp = HttpClient::Get(
                "https://wiki.guildwars2.com/api.php?action=parse&page=" + raw.slug + "&prop=text&format=json");
            if (pageResp.empty()) continue;
            try {
                auto jPage = nlohmann::json::parse(pageResp);
                if (!jPage.contains("parse")) continue;
                std::string pageHtml = jPage["parse"]["text"]["*"].get<std::string>();
                static const std::string kItemNeedle = "data-type=\"item\" data-id=\"";
                static const std::string kCurrNeedle = "data-type=\"currency\" data-id=\"";
                auto tryDataId = [&](const std::string& needle, std::unordered_map<std::string, uint32_t>& map) {
                    auto pos = pageHtml.find(needle);
                    if (pos == std::string::npos) return false;
                    pos += needle.size();
                    auto end = pageHtml.find('"', pos);
                    if (end == std::string::npos) return false;
                    try {
                        uint32_t id = (uint32_t)std::stoul(pageHtml.substr(pos, end - pos));
                        if (id > 0) { map[raw.name] = id; return true; }
                    } catch (...) {}
                    return false;
                };
                // Currency preferred: if page has data-type="currency", use that
                if (!tryDataId(kCurrNeedle, nameToCurrencyId))
                    tryDataId(kItemNeedle, nameToItemId);
            } catch (...) {}
        }

        for (auto& raw : rawIngreds) {
            Ingredient ing;
            ing.count      = raw.count;
            ing.name       = raw.name;
            ing.itemId     = nameToItemId.count(raw.name)     ? nameToItemId[raw.name]     : 0;
            ing.currencyId = nameToCurrencyId.count(raw.name) ? nameToCurrencyId[raw.name] : 0;
            // Authoritative override: if this is a known GW2 wallet currency, always
            // classify it as such regardless of what the wiki extlinks returned.
            uint32_t knownCurrId = GetCurrencyIdByName(raw.name);
            if (knownCurrId > 0) {
                ing.currencyId = knownCurrId;
                ing.itemId     = 0;
            }
            out.ingredients.push_back(ing);
        }
    }

    // Write disk cache
    try {
        nlohmann::json jOut;
        jOut["version"]     = kCacheVersion;
        jOut["recipe_id"]   = out.recipeId;
        jOut["ingredients"] = nlohmann::json::array();
        for (auto& ing : out.ingredients)
            jOut["ingredients"].push_back({{"item_id", ing.itemId}, {"currency_id", ing.currencyId}, {"count", ing.count}, {"name", ing.name}});
        std::ofstream f(cachePath);
        f << jOut.dump();
    } catch (...) {}

    return !out.ingredients.empty() || out.recipeId != 0;
}

void RecipeData::WorkerThread() {
    while (s_running) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lk(s_queueMutex);
            s_cv.wait(lk, [&]{ return !s_queue.empty() || !s_running; });
            if (!s_running) break;
            item = s_queue.front();
            s_queue.erase(s_queue.begin());
        }

        RecipeResult result;
        if (FetchAndCache(item.decoId, item.wikiSlug, result)) {
            std::lock_guard<std::mutex> rl(s_readyMutex);
            s_ready.push_back({item.decoId, std::move(result)});
        } else {
            std::lock_guard<std::mutex> lk(s_cacheMutex);
            s_loading[item.decoId] = false;
        }
    }
}

} // namespace TyrianHomeAndGarden
