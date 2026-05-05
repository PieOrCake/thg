#include "DecorationList.h"
#include <map>
#include <algorithm>
#include <cctype>

namespace PlotTwist {

std::vector<DecorationGroup> DecorationList::s_groups;

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static const std::vector<std::string> kHandiworkOrder =
    {"Novice", "Journeyman", "Adept", "Master", "Grandmaster"};

static const std::vector<std::string> kExpansionOrder =
    {"Core", "Heart of Thorns", "Path of Fire",
     "End of Dragons", "Secrets of the Obscure", "Janthir Wilds",
     "Visions of Eternity"};

std::string DecorationList::GetKey(const Decoration& d, GroupBy by) {
    switch (by) {
        case GroupBy::Type:      return d.category.empty()      ? "Other" : d.category;
        case GroupBy::Handiwork: return d.handiworkLevel.empty() ? "Unknown" : d.handiworkLevel;
        case GroupBy::Expansion: return d.expansion.empty()     ? "Other" : d.expansion;
    }
    return "Other";
}

int DecorationList::SortWeight(const std::string& key, GroupBy by) {
    if (by == GroupBy::Handiwork) {
        for (int i = 0; i < (int)kHandiworkOrder.size(); i++)
            if (kHandiworkOrder[i] == key) return i;
        return 99;
    }
    if (by == GroupBy::Expansion) {
        for (int i = 0; i < (int)kExpansionOrder.size(); i++)
            if (kExpansionOrder[i] == key) return i;
        return 50; // festivals / other after main expansions, alphabetical
    }
    return 0; // Type: alphabetical (SortWeight same for all, use key string)
}

void DecorationList::Rebuild(const std::vector<Decoration>& all,
                              GroupBy by, const std::string& filter)
{
    std::string lfilter = ToLower(filter);
    std::map<std::string, std::vector<Decoration>> buckets;

    for (auto& d : all) {
        if (!lfilter.empty() && ToLower(d.name).find(lfilter) == std::string::npos)
            continue;
        buckets[GetKey(d, by)].push_back(d);
    }

    // Sort items within each bucket alphabetically
    for (auto& [key, items] : buckets) {
        std::sort(items.begin(), items.end(),
            [](const Decoration& a, const Decoration& b) { return a.name < b.name; });
    }

    // Sort buckets
    std::vector<std::pair<std::string, std::vector<Decoration>>> sorted(
        buckets.begin(), buckets.end());
    std::sort(sorted.begin(), sorted.end(),
        [&by](const auto& a, const auto& b) {
            int wa = DecorationList::SortWeight(a.first, by);
            int wb = DecorationList::SortWeight(b.first, by);
            if (wa != wb) return wa < wb;
            return a.first < b.first;
        });

    s_groups.clear();
    for (auto& [key, items] : sorted) {
        DecorationGroup g;
        g.header = key;
        g.items  = std::move(items);
        s_groups.push_back(std::move(g));
    }
}

const std::vector<DecorationGroup>& DecorationList::GetGroups() {
    return s_groups;
}

std::pair<int,int> DecorationList::FindPosition(uint32_t id) {
    for (int gi = 0; gi < (int)s_groups.size(); gi++) {
        for (int ii = 0; ii < (int)s_groups[gi].items.size(); ii++) {
            if (s_groups[gi].items[ii].id == id) return {gi, ii};
        }
    }
    return {-1, -1};
}

} // namespace PlotTwist
