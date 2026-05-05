#include "MetadataScraper.h"
#include "DecorationData.h"
#include "HttpClient.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace PlotTwist {

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
            "?action=query&titles=Homestead_decorations"
            "&prop=revisions&rvprop=ids&format=json");
        if (resp.empty()) return {};
        auto j = nlohmann::json::parse(resp);
        auto& pages = j["query"]["pages"];
        for (auto& [key, page] : pages.items()) {
            return std::to_string(
                page["revisions"][0]["revid"].get<int64_t>());
        }
    } catch (...) {}
    return {};
}

std::string MetadataScraper::CheckNeedsUpdate() {
    // If cache file doesn't exist, always scrape
    std::ifstream cache(s_dataDir + "/decorations_meta.json");
    if (!cache.is_open()) {
        // No cache — need to scrape but we don't have a live rev yet; fetch it
        return FetchLiveRevision(); // may return "" on network error, which is fine
    }
    cache.close();

    // An empty stored revision means the revision file is missing or was never written.
    // This is intentional behavior: without a stored revision we can't know if we're
    // current with the wiki, so we must re-scrape to be safe.
    std::string stored = LoadRevision();
    if (stored.empty()) return FetchLiveRevision();

    std::string live = FetchLiveRevision();
    if (live.empty()) return ""; // network error — keep existing
    if (live == stored) return ""; // up to date
    return live; // needs update — return the revision for saving later
}

// Extract text content from an HTML tag's inner text, stripping child tags.
static std::string InnerText(const std::string& html) {
    std::string result;
    bool inTag = false;
    for (char c : html) {
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (!inTag) result += c;
    }
    // Trim
    size_t s = result.find_first_not_of(" \t\n\r");
    size_t e = result.find_last_not_of(" \t\n\r");
    if (s == std::string::npos) return {};
    return result.substr(s, e - s + 1);
}

// Extract href value from first <a href="..."> in html string.
static std::string ExtractHref(const std::string& html) {
    auto pos = html.find("href=\"");
    if (pos == std::string::npos) return {};
    pos += 6;
    auto end = html.find('"', pos);
    if (end == std::string::npos) return {};
    std::string href = html.substr(pos, end - pos);
    // Strip leading /wiki/
    if (href.substr(0, 6) == "/wiki/") href = href.substr(6);
    return href;
}

bool MetadataScraper::ParseRow(const std::string& row,
    std::string& outName, std::string& outSlug,
    std::string& outHandiwork, std::string& outExpansion)
{
    // Split row into <td>...</td> cells
    std::vector<std::string> cells;
    size_t pos = 0;
    while (true) {
        auto start = row.find("<td", pos);
        if (start == std::string::npos) break;
        auto cStart = row.find('>', start);
        if (cStart == std::string::npos) break;
        auto end = row.find("</td>", cStart);
        if (end == std::string::npos) break;
        cells.push_back(row.substr(cStart + 1, end - cStart - 1));
        pos = end + 5;
    }
    if (cells.size() < 3) return false;

    outName      = InnerText(cells[0]);
    outSlug      = ExtractHref(cells[0]);
    outHandiwork = InnerText(cells[1]);
    outExpansion = InnerText(cells[2]);

    // Normalise handiwork level capitalisation
    if (!outHandiwork.empty())
        outHandiwork[0] = static_cast<char>(std::toupper(
            static_cast<unsigned char>(outHandiwork[0])));

    return !outName.empty();
}

MetaMap MetadataScraper::ScrapeWiki() {
    MetaMap result;

    // Fetch rendered HTML of the decorations page
    auto resp = HttpClient::Get(
        "https://wiki.guildwars2.com/api.php"
        "?action=parse&page=Homestead_decorations&prop=text&format=json");
    if (resp.empty()) return result;

    std::string html;
    try {
        auto j = nlohmann::json::parse(resp);
        html = j["parse"]["text"]["*"].get<std::string>();
    } catch (...) { return result; }

    // Walk through <h2>/<h3> section headers to track current category,
    // then parse <tr> rows within each section's table.
    std::string currentCategory;
    size_t pos = 0;
    while (pos < html.size() && s_running) {
        // Check for section header
        auto h2 = html.find("<h2", pos);
        auto h3 = html.find("<h3", pos);
        auto tr = html.find("<tr", pos);

        // Determine what comes next
        size_t next = std::min({h2, h3, tr});
        if (next == std::string::npos) break;

        if ((next == h2 || next == h3) && next <= tr) {
            // Extract header text
            auto hEnd = html.find("</h", next);
            if (hEnd == std::string::npos) { pos = next + 3; continue; }
            std::string headerHtml = html.substr(next, hEnd - next + 5);
            std::string text = InnerText(headerHtml);
            // Remove edit links "[edit]" that wikis add
            auto bracket = text.find('[');
            if (bracket != std::string::npos) text = text.substr(0, bracket);
            text.erase(std::remove(text.begin(), text.end(), '\n'), text.end());
            auto trim_end = text.find_last_not_of(" \t");
            if (trim_end != std::string::npos) text = text.substr(0, trim_end + 1);
            if (!text.empty()) currentCategory = text;
            pos = hEnd + 5;

        } else if (next == tr) {
            // Extract entire <tr>...</tr>
            auto trEnd = html.find("</tr>", tr);
            if (trEnd == std::string::npos) { pos = tr + 3; continue; }
            std::string row = html.substr(tr, trEnd - tr + 5);
            pos = trEnd + 5;

            std::string name, slug, handiwork, expansion;
            if (!ParseRow(row, name, slug, handiwork, expansion)) continue;

            // Look up decoration ID by name
            const Decoration* d = DecorationData::FindByName(name);
            if (!d) continue;

            result[d->id] = {currentCategory, handiwork, expansion, slug};
        }
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
            {"category",      std::get<0>(t)},
            {"handiworkLevel",std::get<1>(t)},
            {"expansion",     std::get<2>(t)},
            {"wikiSlug",      std::get<3>(t)}
        };
    }
    std::ofstream f(s_dataDir + "/decorations_meta.json");
    if (f.is_open()) f << j.dump(2);
}

void MetadataScraper::WorkerThread() {
    // Always apply cached metadata immediately if available
    auto cached = LoadCache();
    if (!cached.empty()) DecorationData::MergeMetadata(cached);

    std::string liveRev = CheckNeedsUpdate();
    if (liveRev.empty() || !s_running) return;

    auto meta = ScrapeWiki();
    if (meta.empty() || !s_running) return;

    SaveCache(meta);
    SaveRevision(liveRev);  // reuse the already-fetched revision
    DecorationData::MergeMetadata(meta);
}

} // namespace PlotTwist
