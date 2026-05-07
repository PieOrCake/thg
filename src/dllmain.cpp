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
#include <unordered_map>
#include <unordered_set>

#include "nexus/Nexus.h"
#include "imgui.h"
#include <nlohmann/json.hpp>

#include "DecorationData.h"
#include "DecorationList.h"
#include "WikiPreview.h"
#include "IconCache.h"
#include "HandiworkHook.h"
#include "MetadataScraper.h"
#include "RecipeData.h"
#include "HoardAndSeekAPI.h"
#include "icon_data.h"

#define V_MAJOR    0
#define V_MINOR    9
#define V_BUILD    0
#define V_REVISION 0

#define KB_TOGGLE    "KB_THG_TOGGLE"
#define KB_HANDIWORK "KB_THG_HANDIWORK"
#define QA_ID        "QA_THG"
#define TEX_ICON     "TEX_THG_ICON"
#define TEX_ICON_HOV "TEX_THG_ICON_HOV"

namespace TyrianHomeAndGarden {

AddonAPI_t* APIDefs        = nullptr;
bool        g_WindowVisible = false;
std::string g_DataDir;

// --- Render state ---
static int   g_GroupBy     = 0; // 0=Type 1=Handiwork 2=Expansion
static int   g_ViewMode    = 0; // 0=list 1=icons
static float g_SplitRatio  = 0.45f; // list column width as fraction of total
static bool  g_ShowQAIcon  = true;
static char  g_SearchBuf[256] = {};
static std::atomic<bool> g_NeedRebuild{true};

// --- Selection state ---
static uint32_t    g_SelectedId = 0;
static std::string g_SelectedWikiSlug;

// --- Scroll-to state (set by hotkey handler) ---
static bool g_NeedScroll = false;
static int  g_ScrollTargetGi = -1;
static int  g_ScrollTargetIi = -1;

// --- Session persistence ---
static std::unordered_set<std::string> g_CollapsedGroups; // group headers the user has collapsed
static bool                            g_NeedRestoreSelection = false; // scroll + preview after first rebuild

// --- Metadata load state ---
static std::atomic<bool> g_MetaLoaded{false};

// --- Hoard & Seek integration ---
enum { HOARD_ABSENT = 0, HOARD_PENDING = 1, HOARD_DENIED = 2, HOARD_READY = 3 };
static std::atomic<int>      g_HoardState{HOARD_ABSENT};

static std::mutex                            g_OwnedMutex;
static std::unordered_map<uint32_t, int32_t> g_OwnedCounts;    // item_id → owned count
static std::unordered_map<uint32_t, int32_t> g_WalletCounts;   // currency_id → amount
static std::atomic<uint32_t>                 g_OwnedQueryDecoId{0}; // which deco's counts we last queried


// --- Chat link copy feedback ---

static const char* kGroupByLabels[] = {"Type", "Handiwork Level", "Expansion"};

// --- GW2-style ImGui theme (matching Alter Ego) ---
static ImGuiStyle              g_GW2Style;
static std::vector<ImGuiStyle> g_StyleStack;

static void PushGW2Theme() {
    g_StyleStack.push_back(ImGui::GetStyle());
    ImGui::GetStyle() = g_GW2Style;
}

static void PopGW2Theme() {
    if (!g_StyleStack.empty()) {
        ImGui::GetStyle() = g_StyleStack.back();
        g_StyleStack.pop_back();
    }
}

struct ThemeGuard {
    ThemeGuard()  { PushGW2Theme(); }
    ~ThemeGuard() { PopGW2Theme(); }
};

static void BuildGW2Theme() {
    g_GW2Style = ImGui::GetStyle();
    ImGuiStyle& s = g_GW2Style;

    s.WindowRounding    = 6.0f;
    s.ChildRounding     = 4.0f;
    s.FrameRounding     = 4.0f;
    s.PopupRounding     = 4.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding      = 3.0f;
    s.TabRounding       = 4.0f;

    s.WindowPadding    = ImVec2(10, 10);
    s.FramePadding     = ImVec2(6, 4);
    s.ItemSpacing      = ImVec2(8, 5);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.ScrollbarSize    = 12.0f;
    s.GrabMinSize      = 8.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize  = 1.0f;
    s.PopupBorderSize  = 1.0f;
    s.FrameBorderSize  = 0.0f;
    s.TabBorderSize    = 0.0f;

    ImVec4* c = s.Colors;

    c[ImGuiCol_WindowBg]             = ImVec4(0.08f, 0.08f, 0.10f, 0.96f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.07f, 0.07f, 0.09f, 0.80f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.10f, 0.10f, 0.12f, 0.96f);

    c[ImGuiCol_Border]               = ImVec4(0.28f, 0.25f, 0.18f, 0.50f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_FrameBg]              = ImVec4(0.14f, 0.13f, 0.11f, 0.80f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.22f, 0.20f, 0.14f, 0.80f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.28f, 0.25f, 0.16f, 0.90f);

    c[ImGuiCol_TitleBg]              = ImVec4(0.10f, 0.09f, 0.07f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.16f, 0.14f, 0.08f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.08f, 0.07f, 0.05f, 0.75f);

    c[ImGuiCol_MenuBarBg]            = ImVec4(0.12f, 0.11f, 0.09f, 1.00f);

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.06f, 0.06f, 0.07f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.30f, 0.27f, 0.18f, 0.80f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.36f, 0.22f, 0.90f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.50f, 0.44f, 0.26f, 1.00f);

    c[ImGuiCol_CheckMark]            = ImVec4(0.90f, 0.75f, 0.25f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.70f, 0.58f, 0.20f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.85f, 0.70f, 0.25f, 1.00f);

    c[ImGuiCol_Button]               = ImVec4(0.22f, 0.20f, 0.12f, 0.80f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.35f, 0.30f, 0.14f, 0.90f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.45f, 0.38f, 0.16f, 1.00f);

    c[ImGuiCol_Header]               = ImVec4(0.18f, 0.16f, 0.10f, 0.70f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.28f, 0.24f, 0.12f, 0.80f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.35f, 0.30f, 0.14f, 0.90f);

    c[ImGuiCol_Separator]            = ImVec4(0.28f, 0.25f, 0.18f, 0.40f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.50f, 0.42f, 0.20f, 0.70f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.65f, 0.55f, 0.25f, 1.00f);

    c[ImGuiCol_ResizeGrip]           = ImVec4(0.30f, 0.27f, 0.18f, 0.30f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.50f, 0.44f, 0.26f, 0.60f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.65f, 0.55f, 0.25f, 0.90f);

    c[ImGuiCol_Tab]                  = ImVec4(0.14f, 0.13f, 0.10f, 0.86f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.35f, 0.30f, 0.14f, 0.90f);
    c[ImGuiCol_TabActive]            = ImVec4(0.28f, 0.24f, 0.10f, 1.00f);
    c[ImGuiCol_TabUnfocused]         = ImVec4(0.10f, 0.09f, 0.07f, 0.97f);
    c[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.18f, 0.16f, 0.10f, 1.00f);

    c[ImGuiCol_Text]                 = ImVec4(0.90f, 0.87f, 0.78f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.47f, 0.40f, 1.00f);

    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);
    c[ImGuiCol_NavHighlight]         = ImVec4(0.70f, 0.58f, 0.20f, 1.00f);

    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.14f, 0.13f, 0.10f, 1.00f);
    c[ImGuiCol_TableBorderStrong]    = ImVec4(0.28f, 0.25f, 0.18f, 0.60f);
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.22f, 0.20f, 0.15f, 0.40f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(0.10f, 0.10f, 0.08f, 0.30f);

    c[ImGuiCol_PlotHistogram]        = ImVec4(0.65f, 0.55f, 0.15f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.80f, 0.68f, 0.20f, 1.00f);
}

// --- H&S event handlers (called synchronously on the raising thread) ---

#define THG_LOG(lvl, msg) APIDefs->Log(lvl, "THG", msg)
#define THG_LOGF(lvl, ...) do { char _buf[256]; snprintf(_buf, sizeof(_buf), __VA_ARGS__); APIDefs->Log(lvl, "THG", _buf); } while(0)

static void OnHoardPong(void* args) {
    if (!args) { THG_LOG(LOGL_WARNING, "H&S pong received with null args"); return; }
    auto* p = static_cast<HoardPongPayload*>(args);
    THG_LOGF(LOGL_DEBUG, "H&S pong: api_version=%u has_data=%u accounts=%u",
             p->api_version, p->has_data, p->account_count);
    g_HoardState = p->has_data ? HOARD_READY : HOARD_PENDING;
    THG_LOGF(LOGL_INFO, "H&S state -> %s", p->has_data ? "READY" : "PENDING (no data yet)");
}

static void OnHoardDataUpdated(void* args) {
    (void)args;
    THG_LOG(LOGL_INFO, "H&S data updated");
    g_HoardState       = HOARD_READY;
    g_OwnedQueryDecoId = 0;
    {
        std::lock_guard<std::mutex> lk(g_OwnedMutex);
        g_OwnedCounts.clear();
        g_WalletCounts.clear();
    }
}

static void OnOwnedResponse(void* args) {
    if (!args) { THG_LOG(LOGL_WARNING, "OnOwnedResponse: null args"); return; }
    auto* resp = static_cast<HoardQueryItemResponse*>(args);
    THG_LOGF(LOGL_DEBUG, "OnOwnedResponse: item_id=%u status=%u total=%d",
             resp->item_id, resp->status, resp->total_count);
    if (resp->status == HOARD_STATUS_OK) {
        std::lock_guard<std::mutex> lk(g_OwnedMutex);
        g_OwnedCounts[resp->item_id] = resp->total_count;
    } else if (resp->status == HOARD_STATUS_PENDING) {
        THG_LOG(LOGL_INFO, "OnOwnedResponse: PENDING (permission popup shown to user)");
    } else if (resp->status == HOARD_STATUS_DENIED) {
        THG_LOG(LOGL_WARNING, "OnOwnedResponse: DENIED");
        g_HoardState = HOARD_DENIED;
    }
    // H&S owns the response pointer — do not delete
}

static void OnWalletResponse(void* args) {
    if (!args) { THG_LOG(LOGL_WARNING, "OnWalletResponse: null args"); return; }
    auto* resp = static_cast<HoardQueryWalletResponse*>(args);
    THG_LOGF(LOGL_DEBUG, "OnWalletResponse: currency_id=%u status=%u amount=%d",
             resp->currency_id, resp->status, resp->amount);
    if (resp->status == HOARD_STATUS_OK) {
        std::lock_guard<std::mutex> lk(g_OwnedMutex);
        g_WalletCounts[resp->currency_id] = resp->amount;
    } else if (resp->status == HOARD_STATUS_DENIED) {
        THG_LOG(LOGL_WARNING, "OnWalletResponse: DENIED");
        g_HoardState = HOARD_DENIED;
    }
    // H&S owns the response pointer — do not delete
}

// Must be called from the render thread (Events_Raise must be on render thread).
static void QueryIngredientCounts(uint32_t decoId, std::vector<uint32_t> itemIds, std::vector<uint32_t> currencyIds) {
    THG_LOGF(LOGL_DEBUG, "QueryIngredientCounts: decoId=%u items=%zu currencies=%zu",
             decoId, itemIds.size(), currencyIds.size());
    for (auto itemId : itemIds) {
        HoardQueryItemRequest req{};
        req.api_version = HOARD_API_VERSION;
        snprintf(req.requester,      sizeof(req.requester),      "Tyrian Home & Garden");
        snprintf(req.response_event, sizeof(req.response_event), "EV_THG_OWNED_RESPONSE");
        req.item_id = itemId;
        APIDefs->Events_Raise(EV_HOARD_QUERY_ITEM, &req);
    }
    for (auto currencyId : currencyIds) {
        HoardQueryWalletRequest req{};
        req.api_version = HOARD_API_VERSION;
        snprintf(req.requester,      sizeof(req.requester),      "Tyrian Home & Garden");
        snprintf(req.response_event, sizeof(req.response_event), "EV_THG_WALLET_RESPONSE");
        req.currency_id = currencyId;
        APIDefs->Events_Raise(EV_HOARD_QUERY_WALLET, &req);
    }
    g_OwnedQueryDecoId = decoId;
}

static void LoadSettings() {
    std::ifstream f(g_DataDir + "/settings.json");
    if (!f.is_open()) return;
    try {
        auto j = nlohmann::json::parse(f);
        if (j.contains("group_by"))        g_GroupBy      = j["group_by"];
        if (j.contains("window_visible"))  g_WindowVisible = j["window_visible"];
        if (j.contains("view_mode"))       g_ViewMode     = j["view_mode"];
        if (j.contains("split_ratio"))     g_SplitRatio   = j["split_ratio"];
        if (j.contains("show_qa_icon"))    g_ShowQAIcon   = j["show_qa_icon"];
        if (j.contains("selected_id")) {
            g_SelectedId = j["selected_id"].get<uint32_t>();
            if (g_SelectedId != 0) g_NeedRestoreSelection = true;
        }
        if (j.contains("collapsed_groups"))
            for (auto& s : j["collapsed_groups"])
                g_CollapsedGroups.insert(s.get<std::string>());
    } catch (...) {}
}

static void SaveSettings() {
    nlohmann::json j;
    j["group_by"]       = g_GroupBy;
    j["window_visible"]  = g_WindowVisible;
    j["view_mode"]      = g_ViewMode;
    j["split_ratio"]    = g_SplitRatio;
    j["show_qa_icon"]   = g_ShowQAIcon;
    j["selected_id"]    = g_SelectedId;
    j["collapsed_groups"] = nlohmann::json::array();
    for (auto& s : g_CollapsedGroups) j["collapsed_groups"].push_back(s);
    std::ofstream f(g_DataDir + "/settings.json");
    if (f.is_open()) f << j.dump(2);
}

static void ProcessKeybind(const char* id, bool isRelease) {
    if (std::string(id) == KB_TOGGLE) {
        if (!isRelease) { // press — fires for both keyboard and QA icon click
            g_WindowVisible = !g_WindowVisible;
            SaveSettings();
        }
    } else if (std::string(id) == KB_HANDIWORK) {
        if (isRelease) HandiworkHook::TriggerHook();
    }
}

static void AddonOptions() {
    ThemeGuard themeGuard;
    ImGui::Text("Tyrian Home & Garden");
    ImGui::Separator();
    ImGui::TextDisabled("Use the Nexus keybind settings to configure hotkeys.");
    ImGui::Spacing();
    ImGui::Text("View mode");
    {
        bool changed = false;
        if (ImGui::RadioButton("List",  g_ViewMode == 0)) { g_ViewMode = 0; changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Icons", g_ViewMode == 1)) { g_ViewMode = 1; changed = true; }
        if (changed) SaveSettings();
    }
    ImGui::Spacing();
    {
        bool qa = g_ShowQAIcon;
        if (ImGui::Checkbox("Show quick-access icon", &qa)) {
            g_ShowQAIcon = qa;
            if (qa)
                APIDefs->QuickAccess_Add(QA_ID, TEX_ICON, TEX_ICON_HOV, KB_TOGGLE, "Tyrian Home & Garden");
            else
                APIDefs->QuickAccess_Remove(QA_ID);
            SaveSettings();
        }
    }
    ImGui::Spacing();
    if (ImGui::Button("Clear cache and reload metadata")) {
        // Delete cache files so next load re-fetches everything
        std::filesystem::remove(g_DataDir + "/decorations_api.json");
        std::filesystem::remove(g_DataDir + "/decorations_meta.json");
        std::filesystem::remove(g_DataDir + "/meta_revision.txt");
        // Restart the data subsystems
        DecorationData::Shutdown();
        MetadataScraper::Shutdown();
        DecorationData::SetOnApiLoaded([]() { g_NeedRebuild = true; });
        DecorationData::SetOnMetaLoaded([]() { g_NeedRebuild = true; });
        DecorationData::Initialize(APIDefs, g_DataDir);
        MetadataScraper::Initialize(APIDefs, g_DataDir);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(fixes missing groups and stale data)");
}

static void AddonRender() {
    WikiPreview::Tick();
    IconCache::Tick();
    RecipeData::Tick();

    // H&S queries on the render thread (Events_Raise must be on render thread)
    if (g_HoardState == HOARD_READY) {
        // JIT owned ingredient counts for the selected decoration
        if (g_SelectedId != 0 && g_OwnedQueryDecoId != g_SelectedId) {
            const RecipeResult* r = RecipeData::GetResult(g_SelectedId);
            if (r && !r->ingredients.empty()) {
                std::vector<uint32_t> ids, currencyIds;
                for (auto& ing : r->ingredients) {
                    if (ing.itemId     > 0) ids.push_back(ing.itemId);
                    if (ing.currencyId > 0) currencyIds.push_back(ing.currencyId);
                }
                uint32_t selId = g_SelectedId;
                g_OwnedQueryDecoId = selId;
                if (!ids.empty() || !currencyIds.empty()) QueryIngredientCounts(selId, ids, currencyIds);
            }
        }
    }

    // Check for selection from HandiworkHook
    uint32_t pending = HandiworkHook::s_pendingSelectionId.exchange(0);
    if (pending != 0) {
        auto dPendOpt = DecorationData::FindByIdCopy(pending);
        if (dPendOpt) {
            g_SelectedId       = pending;
            g_SelectedWikiSlug = dPendOpt->wikiSlug;
            g_WindowVisible    = true;
            g_OwnedQueryDecoId = 0;
            {
                std::string slug = dPendOpt->wikiSlug;
                if (slug.empty()) {
                    slug = dPendOpt->name;
                    std::replace(slug.begin(), slug.end(), ' ', '_');
                }
                WikiPreview::Request(pending, slug, dPendOpt->iconUrl);
                RecipeData::Request(pending, dPendOpt->wikiSlug);
            }

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

        // Restore selection from previous session (first rebuild after load)
        if (g_NeedRestoreSelection && g_SelectedId != 0) {
            g_NeedRestoreSelection = false;
            auto deco = DecorationData::FindByIdCopy(g_SelectedId);
            if (deco) {
                g_SelectedWikiSlug = deco->wikiSlug;
                g_OwnedQueryDecoId = 0;
                std::string slug = deco->wikiSlug.empty() ? deco->name : deco->wikiSlug;
                std::replace(slug.begin(), slug.end(), ' ', '_');
                WikiPreview::Request(g_SelectedId, slug, deco->iconUrl);
                RecipeData::Request(g_SelectedId, deco->wikiSlug);
            }
            auto [gi, ii] = DecorationList::FindPosition(g_SelectedId);
            if (gi >= 0) {
                g_NeedScroll     = true;
                g_ScrollTargetGi = gi;
                g_ScrollTargetIi = ii;
            }
        }
        // Re-find scroll target after rebuild (group indices may shift)
        else if (g_SelectedId != 0 && g_NeedScroll) {
            auto [gi, ii] = DecorationList::FindPosition(g_SelectedId);
            if (gi >= 0) {
                g_ScrollTargetGi = gi;
                g_ScrollTargetIi = ii;
            }
        }
    }

    if (!g_WindowVisible) return;

    ThemeGuard themeGuard;
    ImGui::SetNextWindowSizeConstraints(ImVec2(480, 320), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin("Tyrian Home & Garden", &g_WindowVisible, ImGuiWindowFlags_NoCollapse)) {
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

    if (DecorationData::IsApiLoaded() && !g_MetaLoaded)
        ImGui::TextDisabled("Loading decoration details...");

    ImGui::Separator();

    const float kSplitterW = 4.0f;
    float avail     = ImGui::GetContentRegionAvail().x;
    float listWidth = std::clamp(g_SplitRatio * avail, 100.0f, avail - 100.0f - kSplitterW);

    // === Left column: grouped list ===
    ImGui::BeginChild("##List", ImVec2(listWidth, 0), false);

    if (!DecorationData::IsApiLoaded()) {
        ImGui::TextDisabled("Loading decorations...");
    } else if (g_ViewMode == 0) {
        // === List mode ===
        const auto& groups = DecorationList::GetGroups();
        for (int gi = 0; gi < (int)groups.size(); gi++) {
            const auto& grp = groups[gi];

            // Force-expand the target group when scrolling to it
            bool forceOpen   = g_NeedScroll && (gi == g_ScrollTargetGi);
            bool wasCollapsed = g_CollapsedGroups.count(grp.header) > 0;
            ImGui::SetNextItemOpen(forceOpen || !wasCollapsed, forceOpen ? ImGuiCond_Always : ImGuiCond_Once);

            std::string header = grp.header + " (" + std::to_string(grp.items.size()) + ")";
            bool open = ImGui::CollapsingHeader(header.c_str());

            // Sync collapsed state and persist if changed
            if (!open && !wasCollapsed) { g_CollapsedGroups.insert(grp.header); SaveSettings(); }
            else if (open && wasCollapsed) { g_CollapsedGroups.erase(grp.header); SaveSettings(); }

            if (!open) continue;

            for (int ii = 0; ii < (int)grp.items.size(); ii++) {
                const auto& item = grp.items[ii];
                bool selected = (item.id == g_SelectedId);

                if (g_NeedScroll && gi == g_ScrollTargetGi && ii == g_ScrollTargetIi) {
                    ImGui::SetScrollHereY(0.5f);
                    g_NeedScroll = false;
                }

                const float kIconSz = 20.0f;
                std::string iconTexId = "THG_ICON_" + std::to_string(item.id);

                IconCache::Request(item.id, item.iconUrl);

                // Invisible selectable for full-row click detection
                float rowX = ImGui::GetCursorPosX();
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.42f, 0.35f, 0.10f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.50f, 0.42f, 0.12f, 1.00f));
                }
                if (ImGui::Selectable(("##" + iconTexId).c_str(), selected,
                                       ImGuiSelectableFlags_None, ImVec2(0, kIconSz))) {
                    g_SelectedId       = item.id;
                    g_SelectedWikiSlug = item.wikiSlug;
                    g_OwnedQueryDecoId = 0;
                    {
                        std::string slug = item.wikiSlug;
                        if (slug.empty()) {
                            slug = item.name;
                            std::replace(slug.begin(), slug.end(), ' ', '_');
                        }
                        WikiPreview::Request(item.id, slug, item.iconUrl);
                        RecipeData::Request(item.id, item.wikiSlug);
                    }
                    SaveSettings();
                }
                if (selected) ImGui::PopStyleColor(2);
                // Render icon and text on top of the selectable row
                ImGui::SameLine(rowX);
                Texture_t* listIcon = IconCache::GetTexture(item.id);
                if (listIcon && listIcon->Resource) {
                    ImGui::Image((ImTextureID)listIcon->Resource, ImVec2(kIconSz, kIconSz));
                } else {
                    ImGui::Dummy(ImVec2(kIconSz, kIconSz));
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(item.name.c_str());
            }
        }
    } else {
        // === Icon grid mode ===
        const float kIconSz  = 48.0f;
        const float kPadding = 6.0f;
        // Each ImageButton is kIconSz + 2*FramePadding wide; subtract scrollbar so rightmost icons aren't clipped
        float framePad = ImGui::GetStyle().FramePadding.x;
        float avail    = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
        float cellSz   = kIconSz + 2.0f * framePad + kPadding;
        int   perRow   = std::max(1, (int)(avail / cellSz));

        const auto& groups = DecorationList::GetGroups();
        for (auto& grp : groups) {
            bool wasCollapsed = g_CollapsedGroups.count(grp.header) > 0;
            ImGui::SetNextItemOpen(!wasCollapsed, ImGuiCond_Once);
            std::string header = grp.header + " (" + std::to_string(grp.items.size()) + ")";
            bool open = ImGui::CollapsingHeader(header.c_str());
            if (!open && !wasCollapsed) { g_CollapsedGroups.insert(grp.header); SaveSettings(); }
            else if (open && wasCollapsed) { g_CollapsedGroups.erase(grp.header); SaveSettings(); }
            if (!open) continue;

            int col = 0;
            for (auto& item : grp.items) {
                IconCache::Request(item.id, item.iconUrl);

                if (col > 0) ImGui::SameLine(0.0f, kPadding);

                bool       selected = (item.id == g_SelectedId);
                ImVec4     bg       = selected ? ImVec4(0.42f, 0.35f, 0.10f, 1.0f) : ImVec4(0,0,0,0);
                Texture_t* tex      = IconCache::GetTexture(item.id);

                bool clicked = false;
                ImGui::PushID((int)item.id);
                if (tex && tex->Resource) {
                    clicked = ImGui::ImageButton((ImTextureID)(void*)tex->Resource,
                                                 ImVec2(kIconSz, kIconSz),
                                                 ImVec2(0,0), ImVec2(1,1), -1, bg);
                } else {
                    ImGui::Dummy(ImVec2(kIconSz, kIconSz));
                    clicked = ImGui::IsItemClicked();
                }
                ImVec2 btnMin = ImGui::GetItemRectMin();
                ImVec2 btnMax = ImGui::GetItemRectMax();
                if (selected)
                    ImGui::GetWindowDrawList()->AddRect(btnMin, btnMax, IM_COL32(180, 148, 40, 255), 2.0f, 0, 2.0f);
                ImGui::PopID();

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", item.name.c_str());

                if (clicked) {
                    g_SelectedId       = item.id;
                    g_SelectedWikiSlug = item.wikiSlug;
                    g_OwnedQueryDecoId = 0;
                    std::string slug = item.wikiSlug;
                    if (slug.empty()) {
                        slug = item.name;
                        std::replace(slug.begin(), slug.end(), ' ', '_');
                    }
                    WikiPreview::Request(item.id, slug, item.iconUrl);
                    RecipeData::Request(item.id, item.wikiSlug);
                    SaveSettings();
                }

                col = (col + 1) % perRow;
            }
        }
    }
    ImGui::EndChild();

    // === Draggable splitter ===
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.17f, 0.14f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.48f, 0.20f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.65f, 0.55f, 0.20f, 1.0f));
    ImGui::Button("##splitter", ImVec2(kSplitterW, -1));
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) {
        float delta = ImGui::GetIO().MouseDelta.x;
        g_SplitRatio = std::clamp((listWidth + delta) / avail, 0.1f, 0.9f);
    }
    if (ImGui::IsItemDeactivated())
        SaveSettings();

    // === Right column: preview ===
    ImGui::SameLine(0, 0);
    ImGui::BeginChild("##Preview", ImVec2(0, 0), false);

    if (g_SelectedId == 0) {
        ImGui::TextDisabled("Select a decoration to preview");
    } else {
        auto dOpt = DecorationData::FindByIdCopy(g_SelectedId);
        if (dOpt) {
            const Decoration& d = *dOpt;
            ImGui::TextColored(ImVec4(0.53f, 0.67f, 1.0f, 1.0f), "%s", d.name.c_str());
            ImGui::Separator();

            bool isHandiwork = !d.handiworkLevel.empty() && !d.wikiSlug.empty();
            const RecipeResult* recipe = isHandiwork ? RecipeData::GetResult(d.id) : nullptr;

            Texture_t* wikiTex = WikiPreview::GetTexture(d.id);
            if (wikiTex && wikiTex->Resource) {
                float availW = ImGui::GetContentRegionAvail().x;
                // Reserve height for metadata rows below the image
                float lh = ImGui::GetTextLineHeightWithSpacing();
                float metaH = 5.0f * lh; // Type, Handiwork, Expansion, wiki button
                if (isHandiwork) {
                    metaH += 2.0f * lh;  // Materials header + status/loading line
                    metaH += recipe ? (float)recipe->ingredients.size() * lh : 1.0f * lh;
                }
                float availH = std::max(ImGui::GetContentRegionAvail().y - metaH, 64.0f);
                float imgW, imgH;
                if (wikiTex->Width > 0 && wikiTex->Height > 0) {
                    float scale = std::min(availW / (float)wikiTex->Width,
                                           availH / (float)wikiTex->Height);
                    imgW = wikiTex->Width  * scale;
                    imgH = wikiTex->Height * scale;
                } else {
                    imgW = imgH = std::min(availW, availH);
                }
                ImGui::Image((ImTextureID)wikiTex->Resource, ImVec2(imgW, imgH));
            } else {
                ImGui::Dummy(ImVec2(64, 64));
                if (WikiPreview::IsLoading(d.id)) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Loading preview...");
                }
            }

            ImGui::Spacing();
            if (!d.category.empty()) {
                ImGui::TextDisabled("Type:");       ImGui::SameLine();
                ImGui::Text("%s", d.category.c_str());
            }
            if (!d.handiworkLevel.empty()) {
                ImGui::TextDisabled("Handiwork:");  ImGui::SameLine();
                ImGui::Text("%s", d.handiworkLevel.c_str());
            }
            if (!d.expansion.empty()) {
                ImGui::TextDisabled("Expansion:");  ImGui::SameLine();
                ImGui::Text("%s", d.expansion.c_str());
            }
            if (!d.wikiSlug.empty()) {
                ImGui::Spacing();
                if (ImGui::SmallButton("Open wiki page")) {
                    std::string url = "https://wiki.guildwars2.com/wiki/" + d.wikiSlug;
                    ShellExecuteA(nullptr, "open", url.c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                }
            }

            // === Recipe section (only for Handiwork-craftable decorations) ===
            if (isHandiwork) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextDisabled("Materials");
                ImGui::Separator();

                int hoardState = g_HoardState.load();

                if (!recipe) {
                    ImGui::TextDisabled("(loading recipe...)");
                } else if (recipe->ingredients.empty()) {
                    ImGui::TextDisabled("(no ingredients)");
                } else {
                    for (auto& ing : recipe->ingredients) {
                        if (hoardState == HOARD_READY) {
                            int32_t owned = 0;
                            {
                                std::lock_guard<std::mutex> lk(g_OwnedMutex);
                                if (ing.currencyId > 0) {
                                    auto it = g_WalletCounts.find(ing.currencyId);
                                    if (it != g_WalletCounts.end()) owned = it->second;
                                } else {
                                    auto it = g_OwnedCounts.find(ing.itemId);
                                    if (it != g_OwnedCounts.end()) owned = it->second;
                                }
                            }
                            bool enough = (owned >= ing.count);
                            if (enough)
                                ImGui::TextDisabled("%d/%d", owned, ing.count);
                            else
                                ImGui::Text("%d/%d", owned, ing.count);
                        } else {
                            ImGui::TextDisabled("%dx", ing.count);
                        }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(ing.name.c_str());
                    }
                }

                // H&S status footer
                ImGui::Spacing();
                switch (hoardState) {
                    case HOARD_ABSENT:
                        ImGui::TextDisabled("Install Hoard & Seek to see\nowned counts and recipe status.");
                        break;
                    case HOARD_PENDING:
                        ImGui::TextDisabled("Waiting for Hoard & Seek\npermission...");
                        break;
                    case HOARD_DENIED:
                        ImGui::TextDisabled("Hoard & Seek permission denied.");
                        break;
                    case HOARD_READY:
                        break;
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

    BuildGW2Theme();

    g_DataDir = APIDefs->Paths_GetAddonDirectory("TyrianHomeAndGarden");
    std::filesystem::create_directories(g_DataDir);

    LoadSettings();

    APIDefs->GUI_Register(RT_Render, AddonRender);
    APIDefs->GUI_Register(RT_OptionsRender, AddonOptions);
    APIDefs->InputBinds_RegisterWithString(KB_TOGGLE,    ProcessKeybind, "CTRL+SHIFT+P");
    APIDefs->InputBinds_RegisterWithString(KB_HANDIWORK, ProcessKeybind, "CTRL+SHIFT+H");
    APIDefs->GUI_RegisterCloseOnEscape("Tyrian Home & Garden", &g_WindowVisible);

    DecorationData::SetOnApiLoaded([]() { g_NeedRebuild = true; });
    DecorationData::SetOnMetaLoaded([]() {
        g_NeedRebuild = true;
        g_MetaLoaded  = true;
        RecipeData::StartPreloader(); // batch-fetch recipe IDs for all Handiwork decorations
    });
    DecorationData::Initialize(APIDefs, g_DataDir);
    MetadataScraper::Initialize(APIDefs, g_DataDir);
    WikiPreview::Initialize(APIDefs, g_DataDir);
    IconCache::Initialize(APIDefs, g_DataDir);
    RecipeData::Initialize(APIDefs, g_DataDir);
    HandiworkHook::Initialize(APIDefs);

    // Quick-access icon — load both textures then register if enabled
    APIDefs->Textures_LoadFromMemory(TEX_ICON,     (void*)ICON_NORMAL, ICON_NORMAL_size, nullptr);
    APIDefs->Textures_LoadFromMemory(TEX_ICON_HOV, (void*)ICON_HOVER,  ICON_HOVER_size,  nullptr);
    if (g_ShowQAIcon)
        APIDefs->QuickAccess_Add(QA_ID, TEX_ICON, TEX_ICON_HOV, KB_TOGGLE, "Tyrian Home & Garden");

    // Hoard & Seek integration — subscribe before pinging so pong is delivered
    APIDefs->Events_Subscribe(EV_HOARD_PONG,         OnHoardPong);
    APIDefs->Events_Subscribe(EV_HOARD_DATA_UPDATED, OnHoardDataUpdated);
    APIDefs->Events_Subscribe("EV_THG_OWNED_RESPONSE",  OnOwnedResponse);
    APIDefs->Events_Subscribe("EV_THG_WALLET_RESPONSE", OnWalletResponse);
    THG_LOG(LOGL_INFO, "Pinging Hoard & Seek...");
    APIDefs->Events_Raise(EV_HOARD_PING, nullptr);
    THG_LOGF(LOGL_INFO, "H&S ping done — state after ping: %d (0=absent,1=pending,2=denied,3=ready)",
             g_HoardState.load());
}

void AddonUnload() {
    SaveSettings();
    if (g_ShowQAIcon) APIDefs->QuickAccess_Remove(QA_ID);
    APIDefs->Events_Unsubscribe("EV_THG_WALLET_RESPONSE", OnWalletResponse);
    APIDefs->Events_Unsubscribe("EV_THG_OWNED_RESPONSE",  OnOwnedResponse);
    APIDefs->Events_Unsubscribe(EV_HOARD_DATA_UPDATED,   OnHoardDataUpdated);
    APIDefs->Events_Unsubscribe(EV_HOARD_PONG,           OnHoardPong);
    HandiworkHook::Shutdown();
    RecipeData::Shutdown();
    IconCache::Shutdown();
    WikiPreview::Shutdown();
    MetadataScraper::Shutdown();
    DecorationData::Shutdown();
    APIDefs->InputBinds_Deregister(KB_HANDIWORK);
    APIDefs->InputBinds_Deregister(KB_TOGGLE);
    APIDefs->GUI_Deregister(AddonOptions);
    APIDefs->GUI_Deregister(AddonRender);
}

} // namespace TyrianHomeAndGarden

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef() {
    static AddonDefinition_t def = {};
    def.Signature   = 0x504C5457;
    def.APIVersion  = NEXUS_API_VERSION;
    def.Name        = "Tyrian Home & Garden";
    def.Version     = {V_MAJOR, V_MINOR, V_BUILD, V_REVISION};
    def.Author      = "PieOrCake.7635";
    def.Description = "Browse homestead decorations with wiki previews";
    def.Load        = TyrianHomeAndGarden::AddonLoad;
    def.Unload      = TyrianHomeAndGarden::AddonUnload;
    def.Flags       = AF_None;
    def.Provider    = UP_GitHub;
    def.UpdateLink  = "https://github.com/PieOrCake/thg";
    return &def;
}
