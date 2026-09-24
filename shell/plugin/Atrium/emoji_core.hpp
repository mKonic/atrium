#pragma once
// The emoji picker's data: Unicode's emoji-test.txt, parsed, and searched by
// name. Plain C++ so it is tested without Qt.

#include <string>
#include <string_view>
#include <vector>

namespace atrium::emoji {

struct Emoji {
    std::string text;   // UTF-8, as it is typed
    std::string name;   // "grinning face"
    std::string group;  // "Smileys & Emotion"
    char32_t first = 0; // its first code point, for font coverage
};

// Fully-qualified emoji in file order, less skin-tone variants and the
// "Component" group (bare modifiers and hair pieces).
std::vector<Emoji> parse(std::string_view emoji_test);

// Indexes into `all` whose name has every word of `query` as the start of
// one of its words ("thumb up" finds "thumbs up"), in file order.
std::vector<size_t> search(const std::vector<Emoji>& all, std::string_view query);

} // namespace atrium::emoji
