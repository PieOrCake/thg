#include "WikiPreview.h"

namespace PlotTwist {

void WikiPreview::Initialize(AddonAPI_t*, const std::string&) {}
void WikiPreview::Shutdown() {}
void WikiPreview::Tick() {}
void WikiPreview::Request(uint32_t, const std::string&) {}
Texture_t* WikiPreview::GetTexture(uint32_t) { return nullptr; }
bool WikiPreview::IsLoading(uint32_t) { return false; }

} // namespace PlotTwist
