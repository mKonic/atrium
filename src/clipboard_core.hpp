#pragma once
// Clipboard history's pure parts: what of a selection is kept, how an entry
// reads in a list, and the newest-first index with its limits.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atrium {

// The type a new selection is recorded as: text if it has any, else a
// picture; nothing for a password manager's copy (it says so with
// x-kde-passwordManagerHint, as KeePassXC and Bitwarden do).
std::optional<std::string> clipboard_mime(const std::vector<std::string>& offered);
bool clipboard_is_text(std::string_view mime);
// What a text entry offers when copied back: the usual names for text.
std::vector<std::string> clipboard_text_mimes();

// One line of it for a list: whitespace runs as one space, at most `max`
// characters (whole UTF-8 sequences).
std::string clipboard_preview(std::string_view text, size_t max = 120);

struct ClipboardEntry {
    std::string id;
    std::string mime;
    int64_t time = 0;   // seconds since the epoch
    uint64_t size = 0;  // bytes
    uint64_t hash = 0;  // of the data, for repeats
    std::string preview;
};

uint64_t clipboard_hash(std::string_view data);

// Newest first. Adding what's already there moves it to the front instead.
class ClipboardIndex {
public:
    std::vector<ClipboardEntry>& entries() { return entries_; }
    const std::vector<ClipboardEntry>& entries() const { return entries_; }
    // The ids that fell off the end (their files are to go), and whether `e`
    // was new (false: an old entry came to the front, its id in `e.id`).
    std::vector<std::string> add(ClipboardEntry& e, bool* fresh, size_t limit = 500);
    const ClipboardEntry* find(std::string_view id) const;
    bool remove(std::string_view id);
    void to_front(std::string_view id);

    std::string to_json() const;
    static ClipboardIndex from_json(std::string_view text);

private:
    std::vector<ClipboardEntry> entries_;
};

} // namespace atrium
