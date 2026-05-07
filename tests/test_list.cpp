// Compiled natively (not cross-compiled) for Linux testing.
// Build with: g++ -std=c++17 -include /tmp/nexus_stub.h -Isrc -Iinclude tests/test_list.cpp src/DecorationList.cpp -o /tmp/test_list
// Where /tmp/nexus_stub.h stubs out Nexus.h for Linux: #ifndef NEXUS_H / #define NEXUS_H / struct AddonAPI_t {}; / struct Texture_t { void* Resource; }; / #endif

// Stub out Nexus.h dependency for test build
#ifndef NEXUS_H
#define NEXUS_H
#include <cstdint>
struct AddonAPI_t {};
struct Texture_t { void* Resource; };
#define NEXUS_API_VERSION 6
#endif

#include "DecorationList.h"
#include <cassert>
#include <iostream>

using namespace TyrianHomeAndGarden;

static Decoration make(uint32_t id, const char* name, const char* cat,
                       const char* hw, const char* exp) {
    Decoration d; d.id=id; d.name=name; d.category=cat;
    d.handiworkLevel=hw; d.expansion=exp; return d;
}

int main() {
    std::vector<Decoration> all = {
        make(1, "Zebra Rug",  "Furniture", "Master",  "Core"),
        make(2, "Apple Tree", "Nature",    "Novice",  "Heart of Thorns"),
        make(3, "Arch, Stone","Architecture","Adept", "Core"),
        make(4, "Candle",     "Lighting",  "Novice",  "Path of Fire"),
        make(5, "Brazier",    "Lighting",  "Journeyman",  "Core"),
    };

    // --- GroupBy::Type, no filter ---
    DecorationList::Rebuild(all, GroupBy::Type, "");
    auto& g = DecorationList::GetGroups();
    assert(g.size() == 4); // Architecture, Furniture, Lighting, Nature
    assert(g[0].header == "Architecture");
    assert(g[2].header == "Lighting");
    assert(g[2].items.size() == 2);
    assert(g[2].items[0].name == "Brazier"); // alphabetical within group
    assert(g[2].items[1].name == "Candle");

    // --- GroupBy::Handiwork, no filter ---
    DecorationList::Rebuild(all, GroupBy::Handiwork, "");
    auto& gh = DecorationList::GetGroups();
    assert(gh[0].header == "Novice");
    assert(gh[1].header == "Journeyman");
    assert(gh[2].header == "Adept");
    assert(gh[3].header == "Master");

    // --- GroupBy::Expansion, no filter ---
    DecorationList::Rebuild(all, GroupBy::Expansion, "");
    auto& ge = DecorationList::GetGroups();
    assert(ge[0].header == "Core");           // Core first
    assert(ge[1].header == "Heart of Thorns");
    assert(ge[2].header == "Path of Fire");

    // --- Filter ---
    DecorationList::Rebuild(all, GroupBy::Type, "arch");
    auto& gf = DecorationList::GetGroups();
    assert(gf.size() == 1);
    assert(gf[0].header == "Architecture");
    assert(gf[0].items[0].name == "Arch, Stone");

    // --- FindPosition ---
    DecorationList::Rebuild(all, GroupBy::Type, "");
    auto [gi, ii] = DecorationList::FindPosition(5); // Brazier in Lighting
    assert(gi == 2 && ii == 0);
    auto [gj, ij] = DecorationList::FindPosition(999);
    assert(gj == -1 && ij == -1);

    std::cout << "All tests passed.\n";
    return 0;
}
