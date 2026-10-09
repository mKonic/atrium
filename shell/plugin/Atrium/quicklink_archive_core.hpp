#pragma once
// Import and Export Quicklinks, as Tinycast's QuicklinkArchive
// (Tinycast/Features/Quicklinks/Model/QuicklinkArchive.swift): a versioned,
// hand-editable JSON file, {"version": 1, "quicklinks": [...]}; a bare array
// reads too, and only name and link are needed (so a Tinycast export
// imports). What's already there, by name or by link, is skipped.

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace atrium::quicklinks {

struct Quicklink {
    std::string name;
    std::string link;  // atrium's records call it url
    std::string app;   // a desktop entry id, or "" for the default
    std::string icon;
    bool root = true;  // listed in the palette's root search
};

constexpr int kVersion = 1;

std::string encode(const std::vector<Quicklink>& quicklinks);

// The quicklinks, or what's wrong with the file.
std::variant<std::vector<Quicklink>, std::string> decode(std::string_view text);

struct Merge {
    std::vector<Quicklink> additions;
    int skipped = 0;
};
// Duplicates by name (any case) or by link, against those there and the rest
// of the file; one with no name or no link is skipped too.
Merge merge(const std::vector<Quicklink>& incoming, const std::vector<Quicklink>& existing);

} // namespace atrium::quicklinks
