#pragma once
#include <string>
#include <cstdint>
#include "nexus/Nexus.h"

namespace PlotTwist {

class WikiPreview {
public:
    static void Initialize(AddonAPI_t* api, const std::string& dataDir);
    static void Shutdown();
    static void Tick();
    static void Request(uint32_t id, const std::string& wikiSlug);
    static Texture_t* GetTexture(uint32_t id);
    static bool IsLoading(uint32_t id);
};

} // namespace PlotTwist
