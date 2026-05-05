#pragma once
#include <atomic>
#include <cstdint>
#include "nexus/Nexus.h"

namespace PlotTwist {

class HandiworkHook {
public:
    static void Initialize(AddonAPI_t* api);
    static void Shutdown();
    static void TriggerHook();

    static std::atomic<uint32_t> s_pendingSelectionId;
};

} // namespace PlotTwist
