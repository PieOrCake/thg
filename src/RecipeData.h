#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include "nexus/Nexus.h"

namespace TyrianHomeAndGarden {

struct Ingredient {
    uint32_t itemId     = 0;  // GW2 item ID (0 if currency or unknown)
    uint32_t currencyId = 0;  // GW2 currency ID (0 if not a currency)
    int      count      = 0;
    std::string name;
};

struct RecipeResult {
    uint32_t recipeId = 0;  // GW2 recipe ID from wiki
    std::vector<Ingredient> ingredients;
};

class RecipeData {
public:
    static void Initialize(AddonAPI_t* api, const std::string& dataDir);
    static void Shutdown();
    static void Tick(); // call from render thread — drains ready queue

    // wikiSlug is the decoration's wiki page slug (e.g. "Academic_Wall").
    // The _(Handiwork) recipe page is derived from it.
    static void              Request(uint32_t decoId, const std::string& wikiSlug);
    static const RecipeResult* GetResult(uint32_t decoId); // nullptr if not yet loaded

    // Lightweight recipe ID lookup — populated from cache and preloader,
    // so available for all Handiwork decorations without full JIT load.
    static uint32_t GetRecipeId(uint32_t decoId);

    // Returns true if any cached ingredient for decoId has a name containing lfilter
    // (case-insensitive substring). lfilter must already be lowercase.
    static bool DecoMatchesIngredient(uint32_t decoId, const std::string& lfilter);

    // Background-activity queries for the status bar
    static bool IsLoadingRecipe(uint32_t decoId);
    static bool IsPreloading();
    static bool IsIndexBuilding();
    // Returns index-build progress as a 0.0–1.0 fraction (only meaningful while IsIndexBuilding()).
    static float GetIndexBuildProgress();

    // Starts background preloader that batch-fetches recipe IDs for all
    // Handiwork decorations. Call once after both API and metadata are loaded.
    static void StartPreloader();

private:
    static void WorkerThread();
    static void PreloaderThread();
    static bool FetchAndCache(uint32_t decoId, const std::string& wikiSlug, RecipeResult& out);
    static void SaveRecipeIdCache();
    static uint32_t ParseRecipeIdFromWikitext(const std::string& wikitext);

    struct QueueItem { uint32_t decoId; std::string wikiSlug; };
    struct ReadyItem { uint32_t decoId; RecipeResult result; };

    // JIT full recipe data
    static AddonAPI_t*              s_api;
    static std::string              s_dataDir;
    static std::atomic<bool>        s_running;
    static std::thread              s_thread;
    static std::mutex               s_queueMutex;
    static std::condition_variable  s_cv;
    static std::vector<QueueItem>   s_queue;
    static std::mutex               s_readyMutex;
    static std::vector<ReadyItem>   s_ready;
    static std::mutex               s_cacheMutex;
    static std::unordered_map<uint32_t, RecipeResult> s_results;
    static std::unordered_map<uint32_t, bool>         s_loading;

    // Lightweight recipe ID cache (decoId → recipeId), persisted to disk
    static std::mutex                              s_recipeIdMutex;
    static std::unordered_map<uint32_t, uint32_t>  s_recipeIds;
    static std::atomic<bool>                       s_recipeIdCacheDirty;
    static std::thread                             s_preloadThread;
    static std::atomic<bool>                       s_preloaderRunning;

    // Ingredient index: lowercase ingredient name → set of decoIds using it
    static std::mutex                                                       s_ingredientMutex;
    static std::unordered_map<std::string, std::unordered_set<uint32_t>>   s_ingredientIndex;
    static std::unordered_set<uint32_t>                                     s_ingredientIndexedDecoIds;
    static std::atomic<bool>                                                s_ingredientIndexDirty;
    static std::atomic<bool>                                                s_indexBuilding;
    static std::atomic<int>                                                 s_indexBuildDone;
    static std::atomic<int>                                                 s_indexBuildTotal;

    static void IndexResult(uint32_t decoId, const RecipeResult& result);
    static void SaveIngredientIndex();
};

} // namespace TyrianHomeAndGarden
