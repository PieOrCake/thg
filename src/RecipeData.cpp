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

bool RecipeData::FetchAndCache(uint32_t decoId, const std::string& wikiSlug, RecipeResult& out) {
    std::string cachePath = s_dataDir + "/recipes/" + std::to_string(decoId) + ".json";

    // Try disk cache first
    if (std::filesystem::exists(cachePath)) {
        try {
            std::ifstream f(cachePath);
            auto j = nlohmann::json::parse(f);
            out.recipeId = j.value("recipe_id", 0u);
            for (auto& ing : j["ingredients"]) {
                Ingredient i;
                i.itemId = ing.value("item_id", 0u);
                i.count  = ing.value("count", 0);
                i.name   = ing.value("name", "");
                out.ingredients.push_back(i);
            }
            return true;
        } catch (...) {}
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

        // name → itemId (normalized page title == ingredient display name)
        std::unordered_map<std::string, uint32_t> nameToItemId;
        if (!extResp.empty()) {
            try {
                auto j = nlohmann::json::parse(extResp);
                static const std::string kItemPrefix = "https://api.guildwars2.com/v2/items?ids=";
                for (auto& [pageId, page] : j["query"]["pages"].items()) {
                    if (!page.contains("extlinks")) continue;
                    std::string pageTitle2 = page["title"].get<std::string>();
                    for (auto& link : page["extlinks"]) {
                        std::string url = link["*"].get<std::string>();
                        if (url.find(kItemPrefix) == 0) {
                            size_t idStart = kItemPrefix.size();
                            auto idEnd = url.find_first_of("&?", idStart);
                            try {
                                uint32_t id = (uint32_t)std::stoul(
                                    url.substr(idStart, idEnd == std::string::npos ? std::string::npos : idEnd - idStart));
                                nameToItemId[pageTitle2] = id;
                            } catch (...) {}
                            break;
                        }
                    }
                }
            } catch (...) {}
        }

        for (auto& raw : rawIngreds) {
            Ingredient ing;
            ing.count  = raw.count;
            ing.name   = raw.name;
            ing.itemId = nameToItemId.count(raw.name) ? nameToItemId[raw.name] : 0;
            out.ingredients.push_back(ing);
        }
    }

    // Write disk cache
    try {
        nlohmann::json jOut;
        jOut["recipe_id"]   = out.recipeId;
        jOut["ingredients"] = nlohmann::json::array();
        for (auto& ing : out.ingredients)
            jOut["ingredients"].push_back({{"item_id", ing.itemId}, {"count", ing.count}, {"name", ing.name}});
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
