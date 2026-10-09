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

// What an entry is, for Clipboard History's type filter (Tinycast's
// ClipboardFilter): the kinds are exclusive, so a copied URL is a link and
// not text. Derived from the data on demand, never stored.
enum class ClipboardKind { Text, Image, File, Color, Link, Email };
const char* clipboard_kind_name(ClipboardKind kind);  // "text", "image", ...
// `data` is the entry's (only its first 2048 bytes matter). Files are
// text of file:// URIs, one per line, as a file manager copies them.
ClipboardKind clipboard_kind(std::string_view mime, std::string_view data);

// A colour as CSS writes it (Tinycast's ColorValue): #rgb, #rgba, #rrggbb,
// #rrggbbaa, rgb()/rgba(), hsl()/hsla(), oklch(), comma or space-and-slash.
struct ClipboardColor {
    double red = 0, green = 0, blue = 0, alpha = 1;  // 0..1, sRGB
};
std::optional<ClipboardColor> clipboard_color(std::string_view text);
// #rrggbb, or #rrggbbaa when it isn't opaque.
std::string clipboard_color_hex(const ClipboardColor& c);

struct ClipboardEntry {
    std::string id;
    std::string mime;
    int64_t time = 0;   // seconds since the epoch
    uint64_t size = 0;  // bytes
    uint64_t hash = 0;  // of the data, for repeats
    std::string preview;
    int64_t pinned = 0;  // when it was pinned (seconds since the epoch); 0: it isn't
};

uint64_t clipboard_hash(std::string_view data);

// Newest first. Adding what's already there moves it to the front instead.
// A pinned entry (Tinycast's pins) stays where it is, never falls off the
// end and outlives Clear All; only removing it drops it.
class ClipboardIndex {
public:
    std::vector<ClipboardEntry>& entries() { return entries_; }
    const std::vector<ClipboardEntry>& entries() const { return entries_; }
    // cliphist's limits: at most `limit` unpinned entries, and the same bytes
    // as the same type among the newest `dedupe` (or any pin) move to the
    // front instead.
    static constexpr size_t kLimit = 750, kDedupe = 100;
    // The ids that fell off the end (their files are to go), and whether `e`
    // was new (false: an old entry came to the front, or a pin stayed, its id
    // in `e.id`).
    std::vector<std::string> add(ClipboardEntry& e, bool* fresh, size_t limit = kLimit);
    const ClipboardEntry* find(std::string_view id) const;
    bool remove(std::string_view id);
    void to_front(std::string_view id);
    // Pinned at `time`, or unpinned; false when there's no such entry.
    bool set_pinned(std::string_view id, bool pinned, int64_t time);
    // Everything but the pins; the ids that went.
    std::vector<std::string> clear_unpinned();
    // As a list shows them: the pins in the order they were pinned, then the
    // rest, newest first.
    std::vector<const ClipboardEntry*> listed() const;

    std::string to_json() const;
    static ClipboardIndex from_json(std::string_view text);

private:
    std::vector<ClipboardEntry> entries_;
};

} // namespace atrium
