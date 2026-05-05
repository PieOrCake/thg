#include "HandiworkHook.h"

namespace PlotTwist {

std::atomic<uint32_t> HandiworkHook::s_pendingSelectionId{0};

void HandiworkHook::Initialize(AddonAPI_t*) {}
void HandiworkHook::Shutdown() {}
void HandiworkHook::TriggerHook() {}

} // namespace PlotTwist
