#pragma once
// Clipboard history's pure parts: what of a selection is kept, how an entry
// reads in a list, and the newest-first index with its limits.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atrium {

// The rules of the tools this replaces: cliphist fed by `wl-paste --type text
// --watch` and `wl-paste --type image --watch` (history), and wl-clip-persist
// (copies outliving their app).

// A password manager's copy (it offers x-kde-passwordManagerHint, as
// KeePassXC and Bitwarden do): neither recorded nor kept.
bool clipboard_sensitive(const std::vector<std::string>& offered);
// What of a copy goes into the history, each its own entry: its text, the
// type `wl-paste --type text` picks (text/plain;charset=utf-8, text/plain,
// then the first textual type), and its picture, the first image/* type.
std::vector<std::string> clipboard_history_mimes(const std::vector<std::string>& offered);
// wl-clipboard's test for a textual type (text/*, the X11 names, json,
// xml, yaml, csv, scripts...).
bool clipboard_is_text(std::string_view mime);
// Whether cliphist would store it: not over 5 MB, not only whitespace.
bool clipboard_worth_recording(std::string_view data);
// The types an entry is offered as when it goes back: plain text under
// every name for plain text (as wl-copy does), anything else as itself.
std::vector<std::string> clipboard_offer_mimes(std::string_view mime);
// The types of a copy read to keep it after its app quits: all of them,
// once each, but SAVE_TARGETS (Firefox never finishes writing it, as
// wl-clip-persist found).
std::vector<std::string> clipboard_persist_mimes(const std::vector<std::string>& offered);

// One line of it for a list, as cliphist previews: whitespace runs as one
// space, at most `max` characters (whole UTF-8 sequences), then "…".
std::string clipboard_preview(std::string_view text, size_t max = 100);

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
    // cliphist's limits: at most `limit` entries, and the same bytes as the
    // same type among the newest `dedupe` move to the front instead.
    static constexpr size_t kLimit = 750, kDedupe = 100;
    // The ids that fell off the end (their files are to go), and whether `e`
    // was new (false: an old entry came to the front, its id in `e.id`).
    std::vector<std::string> add(ClipboardEntry& e, bool* fresh, size_t limit = kLimit);
    const ClipboardEntry* find(std::string_view id) const;
    bool remove(std::string_view id);
    void to_front(std::string_view id);

    std::string to_json() const;
    static ClipboardIndex from_json(std::string_view text);

private:
    std::vector<ClipboardEntry> entries_;
};

} // namespace atrium
