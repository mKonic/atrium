#pragma once
// Clipboard history, kept by the compositor (it sees every selection): each
// new copy (text or a picture, never a password manager's) is read from its
// app and kept in $XDG_STATE_HOME/atrium/clipboard, newest first, for the
// Super+V picker over IPC. It also keeps the clipboard when the app that
// owned it quits, as macOS does: atrium offers the copy from then on.
#include "clipboard_core.hpp"
#include "common.hpp"
#include "wl/signal.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace atrium {

class Server;
namespace wl {
class DataSource;
}

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
    // copies).
    bool add(const std::string& mime, std::string data);
    bool add_file(const std::string& mime, const std::filesystem::path& path);
    bool remove(const std::string& id);
    void clear();

    struct Source;
    struct Read;
    struct Write;

private:
    void selection_changed(wl::DataSource* source);
    void record(wl::DataSource* source, const std::string& mime);
    void finish_read(bool ok);
    void store(const std::string& mime, std::string data, bool current);
    void offer(const std::string& mime, std::shared_ptr<const std::string> data);
    void write(int fd, std::shared_ptr<const std::string> data);
    void reap_writes();
    void save_index();
    void changed();

    Server& server_;
    std::filesystem::path dir_;
    ClipboardIndex index_;
    std::unique_ptr<Read> read_;
    std::vector<std::unique_ptr<Write>> writes_;
    std::unique_ptr<Source> own_;
    wl::Connection selection_;
    // The selection we last recorded, its app, and whether that app went
    // (then the copy stays, offered by atrium).
    wl::DataSource* recorded_from_ = nullptr;
    wl::Connection recorded_gone_;
    std::string last_id_;
    bool source_gone_ = false;
    wl_event_source* restore_ = nullptr;
    wl_event_source* reap_ = nullptr;
    int counter_ = 0;
};

} // namespace atrium
