#pragma once
// The file chooser's rules, free of Qt so they are tested on their own.
// Specced from GTK 3's GtkFileChooserWidget (gtkfilechooserwidget.c,
// gtkfilechooserentry.c, gtkpathbar.c, gtkplacessidebar.c): the dialog the
// GTK portal showed before atrium had its own.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atrium::chooser {

enum class Action { Open, Save, SelectFolder };

// What the accept button (or Return) does, by how much is picked in the
// list: GTK's should_respond() table.
enum class Respond {
    Nothing,    // stays open
    Answer,     // answers with what's picked (or, choosing a folder, where it is)
    Enter,      // the one folder picked is gone into
    SaveEntry,  // what's typed in the name field decides
};
Respond on_accept(Action action, int picked, bool all_folders, bool all_files);

// Whether a row can be picked and activated: choosing a folder, files are
// shown but greyed.
bool sensitive(Action action, bool is_folder);

// A double click or Return on a row: a folder is gone into (whatever is
// being chosen); a file answers when opening or saving.
enum class Activate { Enter, Answer, Nothing };
Activate on_activate(Action action, bool is_folder, bool sensitive);

// The "Modified" column, as GTK words it: the time today, "Yesterday", the
// weekday within a week, "16 Oct" this year, "16 Oct 2025" before.
// `with_time` (Show Time) leaves today's to the time column.
std::string date_text(std::int64_t secs, std::int64_t now, bool with_time, bool clock24);
std::string time_text(std::int64_t secs, bool clock24);

// A file's size, as g_format_size: "1.2 kB", "3.4 MB" (SI units).
std::string size_text(std::uint64_t bytes);

// The Type column's word for a content type, by its generic icon
// (Nautilus's table): "Image", "Text", "Archive"…; empty when not listed.
std::string category(std::string_view generic_icon);

// What's wrong with a new name for a file or folder in a folder:
// `error` stops it, `warning` lets it through.
struct NameCheck {
    std::string error;
    std::string warning;
};
NameCheck check_name(std::string_view name, bool is_folder);

// The location or save field: its text up to the last "/" (with it), and
// after.
struct Split {
    std::string dir;
    std::string file;
};
Split split(std::string_view text);
// "." "..", "~": a folder by name alone.
bool is_directory_shortcut(std::string_view text);
// The folder the text is in (or is, ending in "/" or a shortcut), against
// `base`; "~" is `home`. Empty for text it can't place (a URI).
std::string directory_for_text(std::string_view text, const std::string& base, const std::string& home);
// The file the text names, the same way.
std::string file_for_text(std::string_view text, const std::string& base, const std::string& home);

// Tab in the location field: the longest start every candidate that begins
// with `typed` shares (folders end in "/").
std::string common_prefix(const std::vector<std::string>& candidates, std::string_view typed);

// GTK's bookmarks file (~/.config/gtk-3.0/bookmarks): "URI [label]" a line.
struct Bookmark {
    std::string uri;
    std::string label;
};
std::vector<Bookmark> parse_bookmarks(std::string_view text);
std::string format_bookmarks(const std::vector<Bookmark>& bookmarks);

// file:// URIs and paths, escaped as GLib does.
std::string uri_for_path(std::string_view path);
std::optional<std::string> path_for_uri(std::string_view uri);

// A recently-used.xbel with `uri` added or bumped (visited and modified
// now, `app` among its applications), as GtkRecentManager writes it.
std::string add_recent(std::string_view xbel, std::string_view uri, std::string_view mime, std::string_view app,
                       std::string_view iso_now);

// The folders recently used files are in, newest first, once each (what
// Recent lists when a folder is being chosen or a file saved).
std::vector<std::string> recent_folders(const std::vector<std::string>& files);

// Sorting the list: GTK's compare functions. Folders first (when on) win
// over the column; then the column.
enum class SortColumn { Name, Size, Type, Modified };
struct SortKey {
    bool is_folder = false;
    std::string name_key;  // collated
    std::uint64_t size = 0;
    std::string type;
    std::int64_t modified = 0;
};
bool sorts_before(const SortKey& a, const SortKey& b, SortColumn column, bool ascending, bool folders_first);

} // namespace atrium::chooser
