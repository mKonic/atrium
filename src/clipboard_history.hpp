#pragma once
// The clipboard, kept by the compositor (it sees every selection), doing
// what cliphist (fed by wl-paste --watch) and wl-clip-persist did:
//
// - History: a new copy's text and its picture are each kept as an entry in
//   $XDG_STATE_HOME/atrium/clipboard, newest first, for the Super+V picker
//   over IPC, with cliphist's limits (clipboard_core.hpp). A password
//   manager's copies never are.
// - Persistence: every type a copy offers is read as it is copied, and when
//   the app that owned it quits, atrium offers the same types from then on.
//   Unlike wl-clip-persist it doesn't take the clipboard over while the app
//   is still there (which broke some apps' own clipboard handling).
// - The first time, what cliphist kept is brought over.
#include "clipboard_core.hpp"
#include "listener.hpp"

#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <memory>
#include <string>
#include <vector>

namespace atrium {

class Server;

class ClipboardHistory {
public:
    explicit ClipboardHistory(Server& server, std::filesystem::path dir);
    ~ClipboardHistory();
    ClipboardHistory(const ClipboardHistory&) = delete;
    ClipboardHistory& operator=(const ClipboardHistory&) = delete;

    const ClipboardIndex& index() const { return index_; }
    std::filesystem::path file(const ClipboardEntry& e) const { return dir_ / e.id; }
    // An entry back on the clipboard (and to the front of the list).
    bool copy(const std::string& id);
    // Text, or a file's contents as `mime`, onto the clipboard (wl-copy's job).
    bool set_text(const std::string& text);
    bool set_file(const std::string& mime, const std::filesystem::path& path);
    // Into the list only, the clipboard left as it is (a phone's older
    // copies, cliphist's).
    bool add(const std::string& mime, std::string data);
    bool add_file(const std::string& mime, const std::filesystem::path& path);
    bool remove(const std::string& id);
    // Kept at the top, past the limit and Clear All (or let go of).
    bool pin(const std::string& id, bool pinned);
    // All but the pins.
    void clear();
    // What an entry is, for the type filter, and a colour's swatch (#rrggbb):
    // worked out once per entry, from its data.
    struct Kind {
        ClipboardKind kind = ClipboardKind::Text;
        std::string color;
    };
    const Kind& kind_of(const ClipboardEntry& e) const;

    using Data = std::shared_ptr<const std::string>;
    using Contents = std::map<std::string, Data>;  // by type
    struct Source;
    struct Capture;
    struct Write;

private:
    void selection_changed(wlr_data_source* source);
    void capture(wlr_data_source* source, const std::vector<std::string>& mimes);
    void finish_capture();
    void keep_if_orphaned();
    void store(const std::string& mime, std::string data, bool current);
    void offer(Contents contents, const std::vector<std::string>& order);
    void write(int fd, Data data);
    void reap_writes();
    void save_index();
    void changed();
    void import_cliphist(std::filesystem::path db);
    void finish_import();

    Server& server_;
    std::filesystem::path dir_;
    ClipboardIndex index_;
    std::unique_ptr<Capture> capture_;
    std::vector<std::unique_ptr<Write>> writes_;
    // atrium's own selection; wlroots destroys it once it's replaced.
    Source* own_ = nullptr;
    Listener<> selection_;
    // The copy last read in full, its app, and whether that app went (then
    // atrium offers what was read).
    mutable std::map<std::string, Kind> kinds_;  // by entry id
    Contents kept_;
    std::vector<std::string> order_;  // its types as offered
    wlr_data_source* kept_from_ = nullptr;
    Listener<> kept_gone_;
    bool source_gone_ = false;
    wl_event_source* restore_ = nullptr;
    wl_event_source* reap_ = nullptr;
    int counter_ = 0;
    // Bringing cliphist's history over: read on a thread of its own (it
    // runs cliphist for every entry), added here once it's done.
    std::thread import_;
    int import_done_ = -1;  // eventfd the thread signals
    wl_event_source* import_source_ = nullptr;
    std::mutex import_mutex_;
    std::vector<std::pair<std::string, std::string>> imported_;  // mime, data; oldest first
};

} // namespace atrium
