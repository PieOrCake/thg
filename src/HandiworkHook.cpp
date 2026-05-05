#include "HandiworkHook.h"
#include "DecorationData.h"
#include "HttpClient.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>
#include <vector>
#include <windows.h>

namespace PlotTwist {

AddonAPI_t*           HandiworkHook::s_api = nullptr;
std::atomic<uint32_t> HandiworkHook::s_pendingSelectionId{0};
std::atomic<bool>     HandiworkHook::s_threadActive{false};

void HandiworkHook::Initialize(AddonAPI_t* api) { s_api = api; }

void HandiworkHook::Shutdown() {
    // Wait up to 600ms for any in-flight detached thread to finish
    int waited = 0;
    while (s_threadActive.load() && waited < 600) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waited += 10;
    }
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

void HandiworkHook::SimulateCtrlClick() {
    INPUT inputs[4] = {};
    // Ctrl down
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    // Left mouse button down
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    // Left mouse button up
    inputs[2].type = INPUT_MOUSE;
    inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    // Ctrl up
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, inputs, sizeof(INPUT));
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

// Base64 decode helper
static std::vector<uint8_t> Base64Decode(const std::string& encoded) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> result;
    int val = 0, valb = -8;
    for (unsigned char c : encoded) {
        if (c == '=') break;
        auto pos = chars.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) + (int)pos;
        valb += 6;
        if (valb >= 0) {
            result.push_back((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    return result;
}

uint32_t HandiworkHook::DecodeChatLink(const std::string& link) {
    // Expected format: [&BASE64]
    if (link.size() < 5) return 0;
    if (link.front() != '[' || link[1] != '&') return 0;
    auto end = link.rfind(']');
    if (end == std::string::npos || end <= 2) return 0;

    auto bytes = Base64Decode(link.substr(2, end - 2));
    if (bytes.size() < 4) return 0;
    if (bytes[0] != 0x02) return 0; // not an item link

    // Item ID: bytes 2-4 as uint24 little-endian (covers IDs up to 16.7M)
    uint32_t itemId = (uint32_t)bytes[2]
                    | ((uint32_t)bytes[3] << 8)
                    | ((uint32_t)(bytes.size() > 4 ? bytes[4] : 0) << 16);
    return itemId;
}

uint32_t HandiworkHook::ResolveItemIdToDecorationId(uint32_t itemId) {
    // Fetch item name from GW2 API, then match against decoration list by name.
    try {
        auto resp = HttpClient::Get(
            "https://api.guildwars2.com/v2/items/" + std::to_string(itemId));
        if (resp.empty()) return 0;
        auto j = nlohmann::json::parse(resp);
        std::string name = j.value("name", "");
        if (name.empty()) return 0;
        const Decoration* d = DecorationData::FindByName(name);
        return d ? d->id : 0;
    } catch (...) { return 0; }
}

void HandiworkHook::TriggerHook() {
    std::thread([]() {
        s_threadActive = true;

        std::string original = SaveClipboard();
        SimulateCtrlClick();
        std::string chatLink = PollClipboard(original, 500);

        if (chatLink.empty()) { RestoreClipboard(original); s_threadActive = false; return; }
        if (chatLink.find("[&") == std::string::npos) {
            RestoreClipboard(original); s_threadActive = false; return;
        }

        // Extract just the [&...] portion if surrounded by other text
        auto linkStart = chatLink.find("[&");
        auto linkEnd   = chatLink.find(']', linkStart);
        if (linkStart == std::string::npos || linkEnd == std::string::npos) {
            RestoreClipboard(original); s_threadActive = false; return;
        }
        chatLink = chatLink.substr(linkStart, linkEnd - linkStart + 1);

        uint32_t itemId = DecodeChatLink(chatLink);
        RestoreClipboard(original);

        if (itemId == 0) { s_threadActive = false; return; }

        uint32_t decoId = ResolveItemIdToDecorationId(itemId);
        if (decoId != 0) s_pendingSelectionId = decoId;
        s_threadActive = false;
    }).detach();
}

} // namespace PlotTwist
