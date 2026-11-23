#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace JiStats {

inline constexpr std::string_view kLocalPathReferencePrefix = "scope:fnv1a64:";

// Keep the original spelling as input so a legacy reference can be matched
// without the source file or directory being available.
inline std::string localPathReference(std::string_view path) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char character : path) {
        hash ^= character;
        hash *= 1099511628211ull;
    }

    std::string reference(kLocalPathReferencePrefix);
    constexpr char digits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        reference += digits[(hash >> shift) & 0xf];
    }
    return reference;
}

} // namespace JiStats
