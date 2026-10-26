#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace entis::guide {
// Validate the bounded subset of the original Cotopha SaveObject wire format
// used by ScenarioObject before invoking its native object loader. This never
// allocates according to file counts and never follows external references.
inline bool IsBoundedNativeState(const std::vector<std::uint8_t>& bytes) {
    struct Reader {
        const std::vector<std::uint8_t>& b;
        size_t p = 0, budget = 100000;
        bool okay = true;
        std::uint32_t number() {
            if (p > b.size() || b.size() - p < 4) { okay = false; return 0; }
            const auto v = std::uint32_t(b[p]) | (std::uint32_t(b[p+1]) << 8) |
                (std::uint32_t(b[p+2]) << 16) | (std::uint32_t(b[p+3]) << 24);
            p += 4; return v;
        }
        void skip(size_t n) { if (p > b.size() || n > b.size() - p) okay = false; else p += n; }
        void string(unsigned limit = 8192) { const auto n = number(); if (n > limit) okay = false; else skip(size_t(n) * 2); }
        void structureName(unsigned depth) {
            const auto n = number();
            if (!okay || n > 128 || p > b.size() || size_t(n) * 2 > b.size() - p) { okay = false; return; }
            std::string name;
            for (unsigned i = 0; i < n; ++i) {
                const auto low = b[p++], high = b[p++];
                if (!low || high || low >= 128) { okay = false; return; }
                name += char(low);
            }
            if (depth == 0) { okay = name == "ScenarioObject"; return; }
            for (const auto* allowed : {"ScenarioObject", "BustupInfo", "CgInfo", "EffectParam", "Flag",
                                        "LogManager", "MoveParam", "Point", "Size", "Vector", "ZoomParam"})
                if (name == allowed) return;
            okay = false;
        }
        void array(unsigned depth) {
            const auto n = number();
            if (n > 65536 || n > budget) { okay = false; return; }
            for (unsigned i = 0; i < n && okay; ++i) object(depth + 1);
            object(depth + 1); number();
        }
        void object(unsigned depth) {
            if (!okay || depth > 32 || !budget-- || p >= b.size()) { okay = false; return; }
            const auto type = b[p++];
            switch (type) {
                case 0: {
                    structureName(depth); if (!okay) return; array(depth);
                    const auto names = number();
                    if (names > 128) { okay = false; return; }
                    for (unsigned i = 0; i < names && okay; ++i) string(128);
                    break;
                }
                // A checkpoint must be a self-contained value. Reference
                // offsets can address the live VM stack/global objects.
                case 1: okay = false; break;
                case 2: array(depth); break;
                case 3: {
                    const auto n = number();
                    if (n > 256 || n > budget) { okay = false; return; }
                    for (unsigned i = 0; i < n && okay; ++i) {
                        const auto size = number();
                        if (size == 0xffffffffu) continue;
                        if (size > 8192) { okay = false; return; }
                        skip(size_t(size) * 2); object(depth + 1);
                    }
                    object(depth + 1); break;
                }
                case 4: skip(16); break;
                case 5: skip(8); break;
                case 6: string(); skip(8); break;
                case 0xff: break;
                default: okay = false;
            }
        }
    } reader{bytes};
    if (bytes.size() < 16 || bytes.size() > 2 * 1024 * 1024 || bytes[0] != 0) return false;
    reader.object(0);
    return reader.okay && reader.p == bytes.size();
}
} // namespace entis::guide
