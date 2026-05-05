#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <fstream>
#include <filesystem>
#include <atomic>
#include <algorithm>

#include "nexus/Nexus.h"
#include "imgui.h"
#include <nlohmann/json.hpp>

#include "DecorationData.h"
#include "DecorationList.h"
#include "WikiPreview.h"
#include "HandiworkHook.h"
#include "MetadataScraper.h"

#define V_MAJOR    0
#define V_MINOR    1
#define V_BUILD    0
#define V_REVISION 0

#define KB_TOGGLE    "KB_PLOT_TWIST_TOGGLE"
#define KB_HANDIWORK "KB_PLOT_TWIST_HANDIWORK"

namespace PlotTwist {

AddonAPI_t* APIDefs        = nullptr;
bool        g_WindowVisible = false;
std::string g_DataDir;

// --- Render state ---
static int   g_GroupBy     = 0; // 0=Type 1=Handiwork 2=Expansion
static char  g_SearchBuf[256] = {};
static std::atomic<bool> g_NeedRebuild{true};

// --- Selection state ---
static uint32_t    g_SelectedId = 0;
static std::string g_SelectedWikiSlug;

// --- Scroll-to state (set by hotkey handler) ---
static bool g_NeedScroll = false;
static int  g_ScrollTargetGi = -1;
static int  g_ScrollTargetIi = -1;

static const char* kGroupByLabels[] = {"Type", "Handiwork Level", "Expansion"};

static void LoadSettings() {
    std::ifstream f(g_DataDir + "/settings.json");
    if (!f.is_open()) return;
    try {
        auto j = nlohmann::json::parse(f);
        if (j.contains("group_by"))      g_GroupBy      = j["group_by"];
        if (j.contains("window_visible")) g_WindowVisible = j["window_visible"];
    } catch (...) {}
}

static void SaveSettings() {
    nlohmann::json j;
    j["group_by"]      = g_GroupBy;
    j["window_visible"] = g_WindowVisible;
    std::ofstream f(g_DataDir + "/settings.json");
    if (f.is_open()) f << j.dump(2);
}

static void ProcessKeybind(const char* id, bool isRelease) {
    if (!isRelease) return;
    if (std::string(id) == KB_TOGGLE) {
        g_WindowVisible = !g_WindowVisible;
        SaveSettings();
    } else if (std::string(id) == KB_HANDIWORK) {
        HandiworkHook::TriggerHook();
    }
}

static void AddonOptions() {
    ImGui::Text("Plot Twist");
    ImGui::Separator();
    ImGui::TextDisabled("Use the Nexus keybind settings to configure hotkeys.");
}

static void AddonRender() {
    WikiPreview::Tick();

    // Check for selection from HandiworkHook
    uint32_t pending = HandiworkHook::s_pendingSelectionId.exchange(0);
    if (pending != 0) {
        const Decoration* d = DecorationData::FindById(pending);
        if (d) {
            g_SelectedId       = pending;
            g_SelectedWikiSlug = d->wikiSlug;
            g_WindowVisible    = true;
            WikiPreview::Request(pending, d->wikiSlug);

            // Find scroll target
            auto [gi, ii] = DecorationList::FindPosition(pending);
            if (gi >= 0) {
                g_NeedScroll     = true;
                g_ScrollTargetGi = gi;
                g_ScrollTargetIi = ii;
            }
        }
    }

    // Rebuild list if flagged
    if (g_NeedRebuild && DecorationData::IsApiLoaded()) {
        DecorationList::Rebuild(
            DecorationData::GetDecorations(),
            static_cast<GroupBy>(g_GroupBy),
            g_SearchBuf);
        g_NeedRebuild = false;

        // Re-find scroll target after rebuild (group indices may shift)
        if (g_SelectedId != 0 && g_NeedScroll) {
            auto [gi, ii] = DecorationList::FindPosition(g_SelectedId);
            if (gi >= 0) {
                g_ScrollTargetGi = gi;
                g_ScrollTargetIi = ii;
            }
        }
    }

    if (!g_WindowVisible) return;

    ImGui::SetNextWindowSizeConstraints(ImVec2(480, 320), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin("Plot Twist", &g_WindowVisible, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // Top toolbar
    bool changed = false;
    ImGui::SetNextItemWidth(180);
    if (ImGui::Combo("##GroupBy", &g_GroupBy, kGroupByLabels, 3)) changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##Search", "Search decorations...",
                                  g_SearchBuf, sizeof(g_SearchBuf))) changed = true;
    if (changed) { g_NeedRebuild = true; }

    ImGui::Separator();

    float avail     = ImGui::GetContentRegionAvail().x;
    float listWidth = avail * 0.45f;

    // === Left column: grouped list ===
    ImGui::BeginChild("##List", ImVec2(listWidth, 0), false);

    if (!DecorationData::IsApiLoaded()) {
        ImGui::TextDisabled("Loading decorations...");
    } else {
        const auto& groups = DecorationList::GetGroups();
        for (int gi = 0; gi < (int)groups.size(); gi++) {
            const auto& grp = groups[gi];

            // Force-expand the target group when scrolling to it
            bool forceOpen = g_NeedScroll && (gi == g_ScrollTargetGi);
            if (forceOpen) ImGui::SetNextItemOpen(true, ImGuiCond_Always);

            // Open by default on first render (ImGuiCond_Once respects user toggle after that)
            else ImGui::SetNextItemOpen(true, ImGuiCond_Once);

            std::string header = grp.header + " (" + std::to_string(grp.items.size()) + ")";
            bool open = ImGui::CollapsingHeader(header.c_str());
            if (!open) continue;

            for (int ii = 0; ii < (int)grp.items.size(); ii++) {
                const auto& item = grp.items[ii];
                bool selected = (item.id == g_SelectedId);

                if (g_NeedScroll && gi == g_ScrollTargetGi && ii == g_ScrollTargetIi) {
                    ImGui::SetScrollHereY(0.5f);
                    g_NeedScroll = false;
                }

                if (ImGui::Selectable(item.name.c_str(), selected)) {
                    g_SelectedId       = item.id;
                    g_SelectedWikiSlug = item.wikiSlug;
                    WikiPreview::Request(item.id, item.wikiSlug);
                }
            }
        }
    }
    ImGui::EndChild();

    // === Divider ===
    ImGui::SameLine();

    // === Right column: preview ===
    ImGui::BeginChild("##Preview", ImVec2(0, 0), false);

    if (g_SelectedId == 0) {
        ImGui::TextDisabled("Select a decoration to preview");
    } else {
        const Decoration* d = DecorationData::FindById(g_SelectedId);
        if (d) {
            ImGui::TextColored(ImVec4(0.53f, 0.67f, 1.0f, 1.0f), "%s", d->name.c_str());
            ImGui::Separator();

            float imgSize = std::min(ImGui::GetContentRegionAvail().x, 200.0f);

            Texture_t* wikiTex = WikiPreview::GetTexture(d->id);
            if (wikiTex) {
                ImGui::Image((ImTextureID)wikiTex->Resource, ImVec2(imgSize, imgSize));
            } else {
                // Show API icon while wiki image loads
                std::string texId = "PT_ICON_" + std::to_string(d->id);
                Texture_t* iconTex = APIDefs->Textures_GetOrCreateFromURL(
                    texId.c_str(), d->iconUrl.c_str(), nullptr);
                if (iconTex)
                    ImGui::Image((ImTextureID)iconTex->Resource, ImVec2(64, 64));
                else
                    ImGui::Dummy(ImVec2(64, 64));

                if (WikiPreview::IsLoading(d->id)) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Loading wiki image...");
                }
            }

            ImGui::Spacing();
            if (!d->category.empty()) {
                ImGui::TextDisabled("Type:");       ImGui::SameLine();
                ImGui::Text("%s", d->category.c_str());
            }
            if (!d->handiworkLevel.empty()) {
                ImGui::TextDisabled("Handiwork:");  ImGui::SameLine();
                ImGui::Text("%s", d->handiworkLevel.c_str());
            }
            if (!d->expansion.empty()) {
                ImGui::TextDisabled("Expansion:");  ImGui::SameLine();
                ImGui::Text("%s", d->expansion.c_str());
            }
            if (!d->wikiSlug.empty()) {
                ImGui::Spacing();
                if (ImGui::SmallButton("Open wiki page")) {
                    std::string url = "https://wiki.guildwars2.com/wiki/" + d->wikiSlug;
                    ShellExecuteA(nullptr, "open", url.c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void AddonLoad(AddonAPI_t* aApi) {
    APIDefs = aApi;
    ImGui::SetCurrentContext((ImGuiContext*)APIDefs->ImguiContext);
    ImGui::SetAllocatorFunctions(
        (void*(*)(size_t, void*))APIDefs->ImguiMalloc,
        (void(*)(void*, void*))APIDefs->ImguiFree);

    g_DataDir = APIDefs->Paths_GetAddonDirectory("PlotTwist");
    std::filesystem::create_directories(g_DataDir);

    LoadSettings();

    APIDefs->GUI_Register(RT_Render, AddonRender);
    APIDefs->GUI_Register(RT_OptionsRender, AddonOptions);
    APIDefs->InputBinds_RegisterWithString(KB_TOGGLE,    ProcessKeybind, "CTRL+SHIFT+P");
    APIDefs->InputBinds_RegisterWithString(KB_HANDIWORK, ProcessKeybind, "CTRL+SHIFT+H");
    APIDefs->GUI_RegisterCloseOnEscape("Plot Twist", &g_WindowVisible);

    DecorationData::SetOnApiLoaded([]() { g_NeedRebuild = true; });
    DecorationData::SetOnMetaLoaded([]() { g_NeedRebuild = true; });
    DecorationData::Initialize(APIDefs, g_DataDir);
    MetadataScraper::Initialize(APIDefs, g_DataDir);
    WikiPreview::Initialize(APIDefs, g_DataDir);
    HandiworkHook::Initialize(APIDefs);
}

void AddonUnload() {
    SaveSettings();
    HandiworkHook::Shutdown();
    WikiPreview::Shutdown();
    MetadataScraper::Shutdown();
    DecorationData::Shutdown();
    APIDefs->InputBinds_Deregister(KB_HANDIWORK);
    APIDefs->InputBinds_Deregister(KB_TOGGLE);
    APIDefs->GUI_Deregister(AddonOptions);
    APIDefs->GUI_Deregister(AddonRender);
}

} // namespace PlotTwist

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef() {
    static AddonDefinition_t def = {};
    def.Signature   = 0x504C5457;
    def.APIVersion  = NEXUS_API_VERSION;
    def.Name        = "Plot Twist";
    def.Version     = {V_MAJOR, V_MINOR, V_BUILD, V_REVISION};
    def.Author      = "";
    def.Description = "Browse homestead decorations with wiki previews";
    def.Load        = PlotTwist::AddonLoad;
    def.Unload      = PlotTwist::AddonUnload;
    def.Flags       = AF_None;
    def.Provider    = UP_GitHub;
    def.UpdateLink  = "";
    return &def;
}
