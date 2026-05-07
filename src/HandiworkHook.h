#pragma once
#include <string>
#include <atomic>
#include "nexus/Nexus.h"

namespace TyrianHomeAndGarden {

class HandiworkHook {
public:
    static void Initialize(AddonAPI_t* api);
    static void Shutdown();

    // Called from ProcessKeybind on KB_HANDIWORK release.
    // Runs on the game thread — spawns a worker thread internally.
    static void TriggerHook();

    // Set by the worker thread when a decoration is identified.
    // AddonRender checks and clears this each frame.
    static std::atomic<uint32_t> s_pendingSelectionId;

private:
    static std::string SaveClipboard();
    static void        RestoreClipboard(const std::string& content);
    static std::string PollClipboard(const std::string& original, int maxMs);

    static AddonAPI_t*        s_api;
    static std::atomic<bool>  s_threadActive;
};

} // namespace TyrianHomeAndGarden
