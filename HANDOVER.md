# Plot Twist — Project Handover

_Last updated: 2026-05-05_

---

## What it is

A Guild Wars 2 Nexus addon (C++ DLL) that lets players browse every homestead decoration with a wiki preview image. Press a configurable hotkey while hovering over a decoration in the in-game Handiwork crafting UI to auto-select and preview it in the window.

---

## Tech stack

| Item | Detail |
|---|---|
| Language | C++17 |
| Framework | Nexus addon API v6 |
| UI | ImGui (full source bundled from alter_ego) |
| HTTP | WinInet via HttpClient (copied from alter_ego) |
| JSON | nlohmann/json (headers-only, from alter_ego) |
| Build | MinGW cross-compile on Linux → Windows DLL |
| Build command | `cmake -B build && cmake --build build` |
| Output | `build/PlotTwist.dll` |

The Nexus framework, ImGui version, and exact build toolchain are all intentionally kept identical to the `../alter_ego/` project, which was used as boilerplate. If alter_ego ever updates its Nexus.h or ImGui, this project should follow.

---

## File map

```
src/
  dllmain.cpp          Entry point, AddonLoad/Unload, main window render, keybinds
  DecorationData.h/.cpp  Decoration struct; GW2 API batch fetch; disk cache; metadata merge
  MetadataScraper.h/.cpp Wiki scrape for category/handiwork/expansion/wikiSlug; revision tracking
  DecorationList.h/.cpp  Pure filter/sort/group logic (also tested on Linux)
  WikiPreview.h/.cpp     JIT async wiki image download; disk cache; texture load via Nexus callback
  HttpClient.h/.cpp      WinInet HTTP wrapper (adapted from alter_ego)
  HandiworkHook.h/.cpp   Hotkey → Ctrl+Click sim → clipboard → chat link decode → item lookup

include/nexus/Nexus.h    Nexus API v6 header (copy of alter_ego's)
lib/imgui/               ImGui source (copy of alter_ego's)
lib/nlohmann/            nlohmann/json (copy of alter_ego's)
PlotTwist.def            DLL export: GetAddonDef
tests/
  test_list.cpp          Linux-native tests for DecorationList (filter/sort/group)
  test_chatlink.cpp      Linux-native tests for the GW2 chat link decoder
```

---

## Architecture overview

```
AddonLoad (game startup)
  ├── DecorationData::Initialize  → background thread: LoadApiCache → live GW2 API fetch
  ├── MetadataScraper::Initialize → background thread: LoadCache → wiki scrape if stale
  ├── WikiPreview::Initialize     → background worker thread for JIT image downloads
  └── HandiworkHook::Initialize   → registers keybind callbacks

AddonRender (every frame, game thread)
  ├── WikiPreview::Tick()         → load ready disk-cached images onto render thread
  ├── check HandiworkHook::s_pendingSelectionId (set by hotkey thread)
  ├── rebuild DecorationList if g_NeedRebuild (atomic bool)
  └── draw: toolbar (search + groupby) | left column (grouped list) | right column (preview)

Background threads (concurrent with render):
  DecorationData::FetchThread   → HTTP batches of 200 → sets s_apiLoaded → fires s_onApiLoaded
  MetadataScraper::WorkerThread → wiki HTML parse → MergeMetadata → fires s_onMetaLoaded
  WikiPreview::WorkerThread     → pageimages API + download → pushes to s_ready queue
  HandiworkHook::TriggerHook    → detached thread per keypress (max 600ms lifetime)
```

### Key rules that must not be broken

- **Never call `Textures_GetOrCreateFromURL` from the render callback** — it crashes. The correct pattern is async download → disk → `Textures_LoadFromFile` with callback. `WikiPreview` does this correctly.
- **`Textures_Get`** (pure lookup, no download) is safe from render.
- `FindByIdCopy` returns `std::optional<Decoration>` (a value copy) — never hold a raw pointer into `s_decorations` across frames because `FetchThread` can replace the vector.
- `g_NeedRebuild` is `std::atomic<bool>` because it is written from background threads and read on the render thread.

---

## Data flow

### Decoration names + icons (GW2 API)
1. `GET /v2/homestead/decorations` → ~1140 IDs
2. `GET /v2/homestead/decorations?ids=1..200` (6 batches) → id, name, iconUrl
3. Cached to `{dataDir}/decorations_api.json`; loaded immediately on subsequent runs while live fetch refreshes in background

### Category / handiwork / expansion / wikiSlug (Wiki scraper)
1. Fetches `https://wiki.guildwars2.com/api.php?action=parse&page=Decoration/Homestead&prop=text&format=json`
2. Parses `<div class="filter-plain f-architecture f-secrets-of-the-obscure f-400 recipe-box">` cards
3. CSS class → metadata mapping is hardcoded in `MetadataScraper.cpp` (helpers `ClassesToCategory`, `ClassesToExpansion`, `ClassesToHandiwork`)
4. Revision-tracked: re-scrapes only if wiki page `lastrevid` changes
5. Cached to `{dataDir}/decorations_meta.json` + `meta_revision.txt`
6. `wikiSlug` = decoration name with spaces → underscores (e.g. "Academic Wall" → `Academic_Wall`)

### Handiwork level mapping
CSS class → level name (hardcoded in MetadataScraper):
- `f-1` → Novice, `f-75` → Journeyman, `f-150` → Adept, `f-225` → Master, `f-300`/`f-400` → Grandmaster

### Wiki preview images (JIT)
1. On decoration select: `WikiPreview::Request(id, wikiSlug, iconUrl)`
2. Worker thread: `GET wiki API pageimages?pithumbsize=256` → thumbnail URL
3. Falls back to `iconUrl` (GW2 render CDN) if wiki has no image
4. Downloaded to `{dataDir}/wiki_images/{id}.jpg` (or `.png`)
5. Disk-cached; subsequent selects serve from disk → `Textures_LoadFromFile`
6. Failure cooldown: 600s before retry

### Hotkey → decoration select
1. `Ctrl+Shift+H` fires `HandiworkHook::TriggerHook` (detached thread)
2. Saves clipboard, simulates Ctrl+Click at cursor via `SendInput`
3. Polls clipboard 10×50ms for a `[&...]` chat link
4. Base64-decodes chat link → item ID (uint24 LE from bytes 2–4)
5. `GET /v2/items/{itemId}` → item name → `DecorationData::FindByName` → decoration ID
6. Sets `s_pendingSelectionId`; render thread picks it up next frame, scrolls to item

---

## Known issues / what's left to do

### 🔴 Active crash (last session ended here)
**"Loading it crashes the game"** on a fresh install (no prior config dir).

Root cause is **not yet identified** — session ended before investigation. Possible candidates:
- `DecorationData::FetchThread` calling `FetchLiveRevision` which calls `CheckNeedsUpdate` which does `FetchLiveRevision` again when no cache exists — check if this creates infinite recursion or crashes on empty JSON response
- `WikiPreview::Initialize` calling `std::filesystem::create_directories` on a path that doesn't exist yet
- `HandiworkHook::s_pendingSelectionId` static member definition missing (check `HandiworkHook.cpp`)
- Any unguarded null pointer dereference on first run when `s_decorations` is empty and `g_NeedRebuild` triggers a `Rebuild` with zero items
- The `Textures_LoadFromURL` for icons being called before `APIDefs` is fully initialised
- The detached thread in `HandiworkHook` spawning during shutdown before `Initialize` has run

**First debugging step:** Add a simple log call (`APIDefs->Log(ELogLevel_DEBUG, "PlotTwist", "AddonLoad start")`) at the very beginning of `AddonLoad` and again after each subsystem Initialize, to identify which call is crashing.

### 🟡 Subheadings / dropdown not working yet
Requires the user to press "Clear cache and reload metadata" in the addon options panel (or manually delete the three files in the addon data dir) to trigger a fresh wiki scrape. Once the scraper runs, grouping by Type / Handiwork / Expansion all work correctly.

### 🟡 MetadataScraper: name matching may miss some decorations
`FindByName` does exact case-sensitive string match. If the wiki uses slightly different names than the GW2 API for some decorations (accented characters, punctuation differences), those decorations get no metadata. Not high priority but worth a fuzzy-match fallback.

### 🟡 No GitHub remote yet
The repo is local only. User said they'd set up a GitHub repo and share the URL — then the remote can be added and a PR opened.

### 🟢 Nice-to-have: QuickAccess icon
The addon has no custom icon in the Nexus quick-access bar (it uses a placeholder). Adding one requires embedding a 32×32 PNG as a byte array in `dllmain.cpp` (same pattern as alter_ego's `ICON_NORMAL[]` array) and registering it with `APIDefs->QuickAccess_Add(...)` in `AddonLoad`.

### 🟢 Nice-to-have: Tooltip on hover
Hovering a list item could show a small tooltip with handiwork level / expansion without needing to click.

### 🟢 Nice-to-have: Fix MetadataScraper for non-Handiwork decorations
Decorations with `f-0` rating (likely store-bought / Black Lion items) get an empty handiwork level string. These show in the "Unknown" group under Handiwork sort. Could label them "N/A" or "Store".

---

## Addon data directory layout

```
{GW2}/addons/PlotTwist/          (path returned by Paths_GetAddonDirectory("PlotTwist"))
  decorations_api.json           id → {name, iconUrl}  (from GW2 API)
  decorations_meta.json          id → {category, handiworkLevel, expansion, wikiSlug}
  meta_revision.txt              last seen wiki page revid (for change detection)
  settings.json                  {group_by, window_visible}
  wiki_images/
    {id}.jpg  or  {id}.png       cached wiki thumbnail images
```

Deleting the whole directory is safe — everything regenerates on next load (takes ~1 min for the first full wiki scrape).

---

## Keybinds (defaults, configurable in Nexus)

| Keybind | Action |
|---|---|
| `Ctrl+Shift+P` | Toggle Plot Twist window |
| `Ctrl+Shift+H` | Identify hovered Handiwork item and select it in window |

---

## Design decisions worth remembering

- **`Textures_LoadFromURL` is safe from render; `Textures_GetOrCreateFromURL` is not.** We discovered this the hard way (game crash). The distinction is: LoadFrom* functions are async fire-and-forget; GetOrCreate* functions try to do synchronous work that isn't allowed in the render callback.
- **Wiki thumbnails are JPEG, not PNG.** The `wiki_images/` cache stores `.jpg` files. The extension matters because Nexus uses it for format detection. The cache check in `WikiPreview::Request` tries both `.png` and `.jpg`.
- **wikiSlug is the DECORATION page** (`Academic_Wall`), not the Handiwork recipe page (`Academic_Wall_(Handiwork)`). The recipe page is what appears in the wiki table's href, but the decoration page is what has the preview image.
- **pageimages API** (`prop=pageimages&pithumbsize=256`) is far more reliable than `prop=images` + `prop=imageinfo` for getting a page's main preview image. One call, direct URL.
- **MetadataScraper uses CSS classes, not HTML tables.** The `Decoration/Homestead` wiki page uses JavaScript-filtered div cards, not `<tr>/<td>`. All metadata (category, expansion, rating) is encoded in CSS class tokens on each `<div class="filter-plain ...">` element.
- **Raw pointer from `FindById` is unsafe** across frames — `FetchThread` can replace `s_decorations`. Use `FindByIdCopy` (returns `std::optional<Decoration>`) everywhere in the render path.

---

## Running the Linux unit tests

```bash
# DecorationList (filter/sort/group logic)
echo 'struct AddonAPI_t{}; struct Texture_t{void*Resource;}; \
typedef int(*ADDON_LOAD)(void*); typedef void(*ADDON_UNLOAD)();' > /tmp/nexus_stub.h
g++ -std=c++17 -Isrc -Iinclude -Ilib/nlohmann -include /tmp/nexus_stub.h \
    tests/test_list.cpp src/DecorationList.cpp -o /tmp/test_list && /tmp/test_list

# Chat link decoder
g++ -std=c++17 tests/test_chatlink.cpp -o /tmp/test_chatlink && /tmp/test_chatlink
```

Both should print "All tests passed."
