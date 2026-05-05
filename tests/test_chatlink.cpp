// Build: g++ -std=c++17 tests/test_chatlink.cpp -o /tmp/test_chatlink

#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <cstdint>

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
        if (valb >= 0) { result.push_back((val >> valb) & 0xFF); valb -= 8; }
    }
    return result;
}

static uint32_t DecodeChatLink(const std::string& link) {
    if (link.size() < 5) return 0;
    if (link.front() != '[' || link[1] != '&') return 0;
    auto end = link.rfind(']');
    if (end == std::string::npos || end <= 2) return 0;
    auto bytes = Base64Decode(link.substr(2, end - 2));
    if (bytes.size() < 4) return 0;
    if (bytes[0] != 0x02) return 0;
    return (uint32_t)bytes[2]
         | ((uint32_t)bytes[3] << 8)
         | ((uint32_t)(bytes.size() > 4 ? bytes[4] : 0) << 16);
}

int main() {
    // Manually construct known chat links for specific item IDs.
    // GW2 item link bytes: 02 01 <id_lo> <id_mid> <id_hi> 00 00
    // encoded as base64 (padding stripped by decoder).

    // Item ID 100 (0x000064): bytes 02 01 64 00 00 00 00 -> AgFkAAAAAA==
    assert(DecodeChatLink("[&AgFkAAAAAA==]") == 100);

    // Item ID 256 (0x000100): bytes 02 01 00 01 00 00 00 -> AgEAAQAAAA==
    assert(DecodeChatLink("[&AgEAAQAAAA==]") == 256);

    // Item ID 70000 (0x011170): bytes 02 01 70 11 01 00 00 -> AgFwEQEAAA==
    assert(DecodeChatLink("[&AgFwEQEAAA==]") == 70000);

    // Non-item link (type byte != 0x02) -> 0
    // bytes: 01 00 00 00 00 00 00 00 -> AQAAAAAA (type=0x01)
    assert(DecodeChatLink("[&AQAAAAAA]") == 0);

    // Garbage -> 0
    assert(DecodeChatLink("not a link") == 0);
    assert(DecodeChatLink("") == 0);

    std::cout << "All chat link tests passed.\n";
    return 0;
}
