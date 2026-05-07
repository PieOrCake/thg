#include "HandiworkHook.h"
#include "DecorationData.h"
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <windows.h>

#define PT_LOG(msg) do { if (HandiworkHook::s_api) HandiworkHook::s_api->Log(LOGL_DEBUG, "TyrianHomeAndGarden/Hook", (msg)); } while(0)

namespace TyrianHomeAndGarden {

// Minimal GW2 MumbleLink layout. uiState bit 5 (0x20) = chat textbox focused.
#pragma pack(push, 1)
struct MumbleContext_t {
    uint8_t  serverAddress[28];
    uint32_t mapId;
    uint32_t mapType;
    uint32_t shardId;
    uint32_t instance;
    uint32_t buildId;
    uint32_t uiState;
};
struct LinkedMem_t {
    uint32_t        uiVersion;
    uint32_t        uiTick;
    float           fAvatarPosition[3];
    float           fAvatarFront[3];
    float           fAvatarTop[3];
    wchar_t         name[256];
    float           fCameraPosition[3];
    float           fCameraFront[3];
    float           fCameraTop[3];
    wchar_t         identity[256];
    uint32_t        context_len;
    MumbleContext_t context;
};
#pragma pack(pop)

AddonAPI_t*           HandiworkHook::s_api = nullptr;
std::atomic<uint32_t> HandiworkHook::s_pendingSelectionId{0};
std::atomic<bool>     HandiworkHook::s_threadActive{false};

void HandiworkHook::Initialize(AddonAPI_t* api) { s_api = api; }

void HandiworkHook::Shutdown() {
    int waited = 0;
    while (s_threadActive.load() && waited < 600) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waited += 10;
    }
}

static bool IsTextboxFocused(AddonAPI_t* api) {
    if (!api) return false;
    auto* mem = static_cast<LinkedMem_t*>(api->DataLink_Get(DL_MUMBLE_LINK));
    return mem && (mem->context.uiState & 0x20) != 0;
}

std::string HandiworkHook::SaveClipboard() {
    std::string result;
    if (!OpenClipboard(nullptr)) return result;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        auto* p = static_cast<wchar_t*>(GlobalLock(h));
        if (p) {
            int len = WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                result.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, p, -1, result.data(), len, nullptr, nullptr);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return result;
}

void HandiworkHook::RestoreClipboard(const std::string& content) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (!content.empty()) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, content.c_str(), -1, nullptr, 0);
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(wchar_t));
        if (hg) {
            auto* p = static_cast<wchar_t*>(GlobalLock(hg));
            MultiByteToWideChar(CP_UTF8, 0, content.c_str(), -1, p, wlen);
            GlobalUnlock(hg);
            SetClipboardData(CF_UNICODETEXT, hg);
        }
    }
    CloseClipboard();
}

std::string HandiworkHook::PollClipboard(const std::string& original, int maxMs) {
    int elapsed = 0;
    while (elapsed < maxMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        elapsed += 50;
        std::string current = SaveClipboard();
        if (current != original && !current.empty()) return current;
    }
    return {};
}

static void SendKey(WORD vk, bool up = false) {
    INPUT inp = {};
    inp.type = INPUT_KEYBOARD;
    inp.ki.wVk = vk;
    if (up) inp.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &inp, sizeof(INPUT));
}

// Send a key using a hardware scan code instead of a virtual key.
// Scan-code events travel through the kernel HID path and may be visible to
// DirectInput, unlike virtual-key SendInput events which only reach Win32.
static void SendScanKey(WORD scan, bool up = false) {
    INPUT inp = {};
    inp.type       = INPUT_KEYBOARD;
    inp.ki.wScan   = scan;
    inp.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
    SendInput(1, &inp, sizeof(INPUT));
}

// Attempt Ctrl+A then Ctrl+C via scan codes.
static void TryCopyChatInputScanCode() {
    // Ctrl+A (select all)
    SendScanKey(0x1D);        // Left Ctrl down
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    SendScanKey(0x1E);        // A down
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    SendScanKey(0x1E, true);  // A up
    SendScanKey(0x1D, true);  // Left Ctrl up
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Ctrl+C (copy)
    SendScanKey(0x1D);        // Left Ctrl down
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    SendScanKey(0x2E);        // C down
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    SendScanKey(0x2E, true);  // C up
    SendScanKey(0x1D, true);  // Left Ctrl up
}

static void WaitForModifiersReleased(int timeoutMs = 2000) {
    int elapsed = 0;
    while (elapsed < timeoutMs) {
        if (!(GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
            !(GetAsyncKeyState(VK_SHIFT)   & 0x8000) &&
            !(GetAsyncKeyState(VK_MENU)    & 0x8000)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        elapsed += 10;
    }
}

// Shift+Click sequence matching item_detail_popups / enigo timing:
// Shift down → sleep → click → sleep (post_key_combination_delay) → Shift up.
// Sending them atomically in one SendInput batch is NOT enough — GW2 needs
// time between the Shift press and the click to register the modifier state.
static void SimulateShiftClick() {
    SendKey(VK_SHIFT);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    INPUT clicks[2] = {};
    clicks[0].type = INPUT_MOUSE; clicks[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    clicks[1].type = INPUT_MOUSE; clicks[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, clicks, sizeof(INPUT));

    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // post-click delay
    SendKey(VK_SHIFT, true);
}


// Decode standard base64 to bytes.
static std::vector<uint8_t> Base64Decode(const std::string& encoded) {
    static const std::string alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> result;
    int val = 0, bits = -8;
    for (unsigned char c : encoded) {
        if (c == '=') break;
        auto pos = alpha.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) | (int)pos;
        bits += 6;
        if (bits >= 0) {
            result.push_back((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return result;
}

static uint32_t ParseChatLinkId(const std::string& text) {
    auto s = text.find("[&");
    if (s == std::string::npos) return 0;
    auto e = text.find(']', s + 2);
    if (e == std::string::npos || e <= s + 2) return 0;
    auto bytes = Base64Decode(text.substr(s + 2, e - s - 2));
    if (bytes.size() < 5) return 0;
    return (uint32_t)bytes[1]
         | ((uint32_t)bytes[2] << 8)
         | ((uint32_t)bytes[3] << 16)
         | ((uint32_t)bytes[4] << 24);
}

static std::string ParseItemName(const std::string& text) {
    auto s = text.find('[');
    if (s == std::string::npos) return {};
    auto e = text.find(']', s + 1);
    if (e == std::string::npos || e <= s + 1) return {};
    if (text[s + 1] == '&') return {};
    std::string name = text.substr(s + 1, e - s - 1);
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back()  == ' ') name.pop_back();
    const std::string prefix = "Recipe: ";
    if (name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix)
        name = name.substr(prefix.size());
    return name;
}

void HandiworkHook::TriggerHook() {
    std::thread([]() {
        s_threadActive = true;
        PT_LOG("TriggerHook: thread started");

        WaitForModifiersReleased(2000);
        PT_LOG("TriggerHook: modifiers released");

        std::string original = SaveClipboard();
        PT_LOG(("TriggerHook: clipboard saved, len=" + std::to_string(original.size())).c_str());

        // Try Shift+Click up to twice, checking MumbleLink for textbox focus
        // after each attempt — same retry logic as item_detail_popups.
        bool chatOpen = false;
        for (int attempt = 0; attempt < 2 && !chatOpen; attempt++) {
            if (attempt > 0) {
                PT_LOG("TriggerHook: retrying Shift+Click");
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            SimulateShiftClick();
            PT_LOG(("TriggerHook: Shift+Click sent (attempt " + std::to_string(attempt + 1) + ")").c_str());

            // Wait up to 1 second for the chat textbox to open.
            for (int w = 0; w < 20 && !chatOpen; w++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                chatOpen = IsTextboxFocused(s_api);
            }
        }

        PT_LOG(chatOpen ? "TriggerHook: textbox focused (chat opened)"
                        : "TriggerHook: WARNING - textbox not focused; proceeding anyway");

        if (!chatOpen) {
            PT_LOG("TriggerHook: ABORT - chat did not open");
            s_threadActive = false;
            return;
        }

        // Give GW2 a moment to fully route keyboard input to the chat textbox.
        // MumbleLink bit may be set slightly before keyboard focus is live.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Attempt Ctrl+A + Ctrl+C via hardware scan codes.
        // Scan-code events travel through the kernel HID path and may reach
        // DirectInput, unlike virtual-key or WM_CHAR approaches which don't.
        PT_LOG("TriggerHook: trying scan-code Ctrl+A + Ctrl+C");
        TryCopyChatInputScanCode();

        std::string clipText = PollClipboard(original, 1000);
        PT_LOG(("TriggerHook: clipboard result, len=" + std::to_string(clipText.size()) + " content='" + clipText + "'").c_str());

        // Clear chat text (Ctrl+A then Backspace via scan codes), then close
        // with Escape via WndProc_SendToGameOnly. Plain SendInput/scan-code
        // Escape is intercepted by Nexus's close-on-escape handler and closes
        // the addon window; WndProc_SendToGameOnly bypasses all Nexus hooks.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        SendScanKey(0x1D);       // Ctrl down
        SendScanKey(0x1E);       // A down
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        SendScanKey(0x1E, true); // A up
        SendScanKey(0x1D, true); // Ctrl up
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        SendScanKey(0x0E);       // Backspace (clears selected text)
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        SendScanKey(0x1C);       // Enter (submits empty chat, closes box; no Nexus handler)

        RestoreClipboard(original);
        PT_LOG("TriggerHook: clipboard restored");

        if (clipText.empty()) {
            PT_LOG("TriggerHook: ABORT - clipboard unchanged (Shift+Click had no effect or chat was empty)");
            s_threadActive = false;
            return;
        }

        // Accept [&base64] links (Handiwork items) and plain [Item Name] (inventory items).
        uint32_t decoId = 0;
        std::string resolvedName;
        if (clipText.find("[&") != std::string::npos) {
            decoId = ParseChatLinkId(clipText);
            resolvedName = "[chat link id=" + std::to_string(decoId) + "]";
            PT_LOG(("TriggerHook: decoded chat link, decoId=" + std::to_string(decoId)).c_str());
        } else {
            resolvedName = ParseItemName(clipText);
            PT_LOG(("TriggerHook: item name='" + resolvedName + "'").c_str());
            if (!resolvedName.empty()) decoId = DecorationData::FindIdByName(resolvedName);
        }

        if (decoId == 0 || !DecorationData::FindByIdCopy(decoId)) {
            PT_LOG(("TriggerHook: ABORT - could not resolve decoration: '" + resolvedName + "'").c_str());
            if (s_api) s_api->GUI_SendAlert(("Not found: " + resolvedName).c_str());
            s_threadActive = false;
            return;
        }

        s_pendingSelectionId = decoId;
        PT_LOG("TriggerHook: pendingSelectionId set, window will open next frame");
        s_threadActive = false;
        PT_LOG("TriggerHook: thread done");
    }).detach();
}

} // namespace TyrianHomeAndGarden
