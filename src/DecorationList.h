#pragma once
#include <string>
#include <vector>
#include "DecorationData.h"

namespace TyrianHomeAndGarden {

enum class GroupBy { Type = 0, Handiwork = 1, Expansion = 2 };

struct DecorationGroup {
    std::string           header;
    std::vector<Decoration> items;
};

class DecorationList {
public:
    // Rebuilds the grouped list from scratch. Filter is case-insensitive substring match on name.
    // Pinned items are excluded from normal groups and prepended as a synthetic "Pinned" group.
    static void Rebuild(const std::vector<Decoration>& all,
                        GroupBy by, const std::string& filter,
                        const std::vector<uint32_t>& pinned);

    static const std::vector<DecorationGroup>& GetGroups();

    // Returns {groupIndex, itemIndex} for the given decoration ID, or {-1,-1} if not found.
    static std::pair<int,int> FindPosition(uint32_t id);

private:
    static std::string GetKey(const Decoration& d, GroupBy by);
    static int         SortWeight(const std::string& key, GroupBy by);

    static std::vector<DecorationGroup> s_groups;
};

} // namespace TyrianHomeAndGarden
