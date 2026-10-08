#include "clipboard_history.hpp"

#include "ipc.hpp"
#include "seat.hpp"
#include "server.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/eventfd.h>
#include <unistd.h>

namespace atrium {

namespace {

// wl-clip-persist's write timeout, used for reading too: a type an app takes
// longer than this to hand over isn't kept.
constexpr int kReadTimeoutMs = 3000;
// The most a picked entry (Super+V) or a file put on the clipboard may be.
constexpr size_t kFileLimit = 64u << 20;

int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::optional<std::string> read_file(const std::filesystem::path& path, size_t limit) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream s;
    s << in.rdbuf();
    std::string data = std::move(s).str();
    if (data.size() > limit)
        return std::nullopt;
    return data;
}

std::vector<std::string> offered_types(wlr_data_source* source) {
    std::vector<std::string> out;
    auto* types = static_cast<char**>(source->mime_types.data);
    for (size_t i = 0; i < source->mime_types.size / sizeof(char*); ++i)
        out.emplace_back(types[i]);
    return out;
}

} // namespace

// atrium's own selection: a history entry, or a copy whose app quit. wlroots
// owns it once it's the selection, and destroys it when it's replaced.
struct ClipboardHistory::Source {
    wlr_data_source base;  // first: a wlr_data_source* is a Source*
    ClipboardHistory* history;
    Contents contents;

    static void send(wlr_data_source* s, const char* mime, int32_t fd) {
        auto* self = reinterpret_cast<Source*>(s);
        auto it = self->contents.find(mime);
        if (self->history && it != self->contents.end())
            self->history->write(fd, it->second);
        else
            close(fd);
    }
    static void destroy(wlr_data_source* s) {
        auto* self = reinterpret_cast<Source*>(s);
        if (self->history && self->history->own_ == self)
            self->history->own_ = nullptr;
        delete self;
    }
    static constexpr wlr_data_source_impl kImpl = {.send = send, .destroy = destroy};

    // In `order`: apps take the first type they understand.
    Source(ClipboardHistory& h, Contents c, const std::vector<std::string>& order) : history(&h), contents(std::move(c)) {
        wlr_data_source_init(&base, &kImpl);
        for (const std::string& m : order)
            if (auto* slot = static_cast<char**>(wl_array_add(&base.mime_types, sizeof(char*))))
                *slot = strdup(m.c_str());
    }
};

// A copy being read from its app: every type it offers, at once.
struct ClipboardHistory::Capture {
    struct Part {
        Capture* capture = nullptr;
        std::string mime, data;
        int fd = -1;
        bool done = false, ok = false;
        wl_event_source* readable = nullptr;
        ~Part() {
            if (readable)
                wl_event_source_remove(readable);
            if (fd >= 0)
                close(fd);
        }
    };
    ClipboardHistory* history = nullptr;
    wlr_data_source* source = nullptr;
    std::vector<std::string> order;  // as offered
    std::vector<std::unique_ptr<Part>> parts;
    wl_event_source* timeout = nullptr;
    Listener<> gone;
    ~Capture() {
        if (timeout)
            wl_event_source_remove(timeout);
    }
    bool finished() const {
        return std::ranges::all_of(parts, [](const auto& p) { return p->done; });
    }
};

// A copy being handed to an app that pastes it.
struct ClipboardHistory::Write {
    ClipboardHistory* history = nullptr;
    int fd = -1;
    Data data;
    size_t done = 0;
    wl_event_source* writable = nullptr;
    ~Write() {
        if (writable)
            wl_event_source_remove(writable);
        if (fd >= 0)
            close(fd);
    }
};

ClipboardHistory::ClipboardHistory(Server& server, std::filesystem::path dir) : server_(server), dir_(std::move(dir)) {
    std::error_code ec;
    const bool first = !std::filesystem::exists(dir_ / "index.json", ec);
    std::filesystem::create_directories(dir_, ec);
    std::filesystem::permissions(dir_, std::filesystem::perms::owner_all, ec);
    if (auto text = read_file(dir_ / "index.json", 64u << 20))
        index_ = ClipboardIndex::from_json(*text);
    // Entries whose file is gone are gone.
    std::erase_if(index_.entries(), [&](const ClipboardEntry& e) { return !std::filesystem::exists(file(e), ec); });
    selection_.connect(&server_.seat->wlr->events.set_selection,
                       [this](void*) { selection_changed(server_.seat->wlr->selection_source); });
    if (first) {
        save_index();
        const char* cache = std::getenv("XDG_CACHE_HOME");
        const char* home = std::getenv("HOME");
        const std::filesystem::path db = cache && *cache ? std::filesystem::path(cache) / "cliphist/db"
                                         : home         ? std::filesystem::path(home) / ".cache/cliphist/db"
                                                        : std::filesystem::path();
        if (!db.empty() && std::filesystem::exists(db, ec))
            import_cliphist(db);
    }
}

ClipboardHistory::~ClipboardHistory() {
    if (import_.joinable())
        import_.join();
    if (import_source_)
        wl_event_source_remove(import_source_);
    if (import_done_ >= 0)
        close(import_done_);
    if (restore_)
        wl_event_source_remove(restore_);
    if (reap_)
        wl_event_source_remove(reap_);
    selection_.disconnect();
    kept_gone_.disconnect();
    capture_.reset();
    writes_.clear();
    // Ours still on the clipboard goes with it; one wlroots still holds
    // elsewhere no longer answers.
    if (own_ && server_.seat->wlr->selection_source == &own_->base)
        wlr_seat_set_selection(server_.seat->wlr, nullptr, wl_display_next_serial(server_.display));
    if (own_)
        own_->history = nullptr;
    own_ = nullptr;
}

void ClipboardHistory::selection_changed(wlr_data_source* source) {
    if (source && own_ && source == &own_->base)
        return;
    if (!source) {
        // Emptied: if that's because its app quit, the copy stays (on the
        // next turn, once the app's going has been seen).
        if (!restore_)
            restore_ = wl_event_loop_add_idle(server_.loop, [](void* d) {
                auto* self = static_cast<ClipboardHistory*>(d);
                self->restore_ = nullptr;
                self->keep_if_orphaned();
            }, this);
        return;
    }
    // A new copy: the last one is no longer ours to keep.
    capture_.reset();
    kept_.clear();
    kept_from_ = nullptr;
    kept_gone_.disconnect();
    source_gone_ = false;
    const std::vector<std::string> offered = offered_types(source);
    // A password manager's copy is left alone: not kept, not recorded.
    if (clipboard_sensitive(offered))
        return;
    capture(source, offered);
}

void ClipboardHistory::capture(wlr_data_source* source, const std::vector<std::string>& offered) {
    auto c = std::make_unique<Capture>();
    c->history = this;
    c->source = source;
    c->order = clipboard_persist_mimes(offered);
    if (c->order.empty())
        return;
    c->gone.connect(&source->events.destroy, [this](void*) {
        // The app went: what it already wrote still arrives (or ends).
        if (capture_) {
            capture_->source = nullptr;
            capture_->gone.disconnect();
        }
        source_gone_ = true;
    });
    for (const std::string& mime : c->order) {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
            continue;
        auto p = std::make_unique<Capture::Part>();
        p->capture = c.get();
        p->mime = mime;
        p->fd = fds[0];
        p->readable = wl_event_loop_add_fd(server_.loop, fds[0], WL_EVENT_READABLE, [](int fd, uint32_t mask, void* d) {
            auto* p = static_cast<Capture::Part*>(d);
            char buf[65536];
            for (;;) {
                const ssize_t n = read(fd, buf, sizeof buf);
                if (n > 0) {
                    p->data.append(buf, size_t(n));
                    continue;
                }
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0 && errno == EAGAIN && !(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)))
                    return 0;
                p->ok = n == 0 || (n < 0 && errno == EAGAIN);  // the end, or hung up after it
                break;
            }
            p->done = true;
            wl_event_source_remove(p->readable);
            p->readable = nullptr;
            close(p->fd);
            p->fd = -1;
            if (p->capture->finished())
                p->capture->history->finish_capture();
            return 0;
        }, p.get());
        wlr_data_source_send(source, mime.c_str(), fds[1]);  // it closes the write end
        c->parts.push_back(std::move(p));
    }
    c->timeout = wl_event_loop_add_timer(server_.loop, [](void* d) {
        auto* self = static_cast<ClipboardHistory*>(d);
        // What hasn't come by now isn't kept.
        for (auto& p : self->capture_->parts)
            p->done = true;
        self->finish_capture();
        return 0;
    }, this);
    wl_event_source_timer_update(c->timeout, kReadTimeoutMs);
    capture_ = std::move(c);
    if (capture_->parts.empty())
        capture_.reset();
}

void ClipboardHistory::finish_capture() {
    if (!capture_)
        return;
    std::unique_ptr<Capture> c = std::move(capture_);
    Contents read;
    for (auto& p : c->parts)
        if (p->ok && !p->data.empty())
            read[p->mime] = std::make_shared<const std::string>(std::move(p->data));
    if (read.empty())
        return;
    // History: its text and its picture, as cliphist's two watchers saw them.
    if (server_.config.clipboard_history) {
        std::vector<std::string> offered;
        for (const std::string& m : c->order)
            if (read.contains(m))
                offered.push_back(m);
        for (const std::string& m : clipboard_history_mimes(offered))
            if (clipboard_worth_recording(*read[m]))
                store(m, *read[m], true);
    }
    // Kept, in the order offered, for when its app goes.
    if (c->source) {
        kept_from_ = c->source;
        kept_gone_.connect(&c->source->events.destroy, [this](void*) {
            source_gone_ = true;
            kept_from_ = nullptr;
            kept_gone_.disconnect();
        });
    }
    kept_ = std::move(read);
    order_ = c->order;
    std::erase_if(order_, [&](const std::string& m) { return !kept_.contains(m); });
    keep_if_orphaned();
}

// The app that owned the clipboard quit and nothing took its place: what it
// offered, offered by atrium.
void ClipboardHistory::keep_if_orphaned() {
    if (server_.seat->wlr->selection_source || !source_gone_ || kept_.empty() || capture_)
        return;
    source_gone_ = false;
    Contents contents = std::move(kept_);
    kept_.clear();
    offer(std::move(contents), order_);
}

bool ClipboardHistory::add(const std::string& mime, std::string data) {
    if (!server_.config.clipboard_history || !clipboard_worth_recording(data))
        return false;
    store(mime, std::move(data), false);
    return true;
}

void ClipboardHistory::store(const std::string& mime, std::string data, bool) {
    ClipboardEntry e;
    e.id = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count()) +
           "-" + std::to_string(++counter_);
    e.mime = mime;
    e.time = now_seconds();
    e.size = data.size();
    e.hash = clipboard_hash(data);
    if (clipboard_is_text(mime))
        e.preview = clipboard_preview(data);
    bool fresh = false;
    for (const std::string& gone : index_.add(e, &fresh)) {
        std::error_code ec;
        std::filesystem::remove(dir_ / gone, ec);
    }
    if (fresh) {
        const auto path = file(e);
        if (std::ofstream out(path, std::ios::binary); out) {
            out.write(data.data(), std::streamsize(data.size()));
            std::error_code ec;
            std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                         ec);
        }
    }
    save_index();
    changed();
}

bool ClipboardHistory::copy(const std::string& id) {
    const ClipboardEntry* e = index_.find(id);
    if (!e)
        return false;
    auto data = read_file(file(*e), kFileLimit);
    if (!data)
        return false;
    const std::string mime = e->mime;
    index_.to_front(id);
    save_index();
    auto shared = std::make_shared<const std::string>(std::move(*data));
    Contents contents;
    const std::vector<std::string> types = clipboard_offer_mimes(mime);
    for (const std::string& m : types)
        contents[m] = shared;
    offer(std::move(contents), types);
    changed();
    return true;
}

bool ClipboardHistory::set_text(const std::string& text) {
    auto shared = std::make_shared<const std::string>(text);
    if (server_.config.clipboard_history && clipboard_worth_recording(text))
        store("text/plain;charset=utf-8", text, true);
    Contents contents;
    const std::vector<std::string> types = clipboard_offer_mimes("text/plain;charset=utf-8");
    for (const std::string& m : types)
        contents[m] = shared;
    offer(std::move(contents), types);
    return true;
}

bool ClipboardHistory::set_file(const std::string& mime, const std::filesystem::path& path) {
    if (mime.empty() || mime.find('/') == std::string::npos)
        return false;
    auto data = read_file(path, kFileLimit);
    if (!data)
        return false;
    auto shared = std::make_shared<const std::string>(*data);
    if (server_.config.clipboard_history && !clipboard_history_mimes({mime}).empty() &&
        clipboard_worth_recording(*data))
        store(mime, std::move(*data), true);
    Contents contents;
    const std::vector<std::string> types = clipboard_offer_mimes(mime);
    for (const std::string& m : types)
        contents[m] = shared;
    offer(std::move(contents), types);
    return true;
}

bool ClipboardHistory::add_file(const std::string& mime, const std::filesystem::path& path) {
    if (clipboard_history_mimes({mime}).empty())
        return false;
    auto data = read_file(path, kFileLimit);
    return data && add(mime, std::move(*data));
}

void ClipboardHistory::offer(Contents contents, const std::vector<std::string>& order) {
    // Ours before it's the selection (so it isn't read as a new copy);
    // wlroots destroys the one it replaces.
    own_ = new Source(*this, std::move(contents), order);
    wlr_seat_set_selection(server_.seat->wlr, &own_->base, wl_display_next_serial(server_.display));
}

void ClipboardHistory::write(int fd, Data data) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    auto w = std::make_unique<Write>();
    w->history = this;
    w->fd = fd;
    w->data = std::move(data);
    Write* raw = w.get();
    w->writable = wl_event_loop_add_fd(server_.loop, fd, WL_EVENT_WRITABLE, [](int fd, uint32_t mask, void* d) {
        auto* w = static_cast<Write*>(d);
        while (w->done < w->data->size()) {
            const ssize_t n = ::write(fd, w->data->data() + w->done, w->data->size() - w->done);
            if (n > 0) {
                w->done += size_t(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && errno == EAGAIN && !(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)))
                return 0;
            break;  // the reader went
        }
        // Done: the reader sees the end now; the rest goes on the next turn
        // (not inside its own callback).
        wl_event_source_remove(w->writable);
        w->writable = nullptr;
        close(w->fd);
        w->fd = -1;
        w->history->reap_writes();
        return 0;
    }, raw);
    writes_.push_back(std::move(w));
}

void ClipboardHistory::reap_writes() {
    if (reap_)
        return;
    reap_ = wl_event_loop_add_idle(server_.loop, [](void* d) {
        auto* self = static_cast<ClipboardHistory*>(d);
        self->reap_ = nullptr;
        std::erase_if(self->writes_, [](const std::unique_ptr<Write>& x) { return x->fd < 0; });
    }, this);
}

bool ClipboardHistory::remove(const std::string& id) {
    if (!index_.remove(id))
        return false;
    std::error_code ec;
    std::filesystem::remove(dir_ / id, ec);
    save_index();
    changed();
    return true;
}

void ClipboardHistory::clear() {
    for (const ClipboardEntry& e : index_.entries()) {
        std::error_code ec;
        std::filesystem::remove(file(e), ec);
    }
    index_.entries().clear();
    save_index();
    changed();
}

void ClipboardHistory::save_index() {
    const auto tmp = dir_ / "index.json.new";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << index_.to_json();
    }
    // Its previews are what was copied: for this user only.
    std::error_code ec;
    std::filesystem::permissions(tmp, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, ec);
    std::filesystem::rename(tmp, dir_ / "index.json", ec);
}

void ClipboardHistory::changed() {
    if (server_.ipc)
        server_.ipc->broadcast("clipboard", {{"event", "clipboard.changed"}});
}

namespace {

// Everything a command prints (nothing if it couldn't start).
std::optional<std::string> run(const std::string& command) {
    FILE* p = popen(command.c_str(), "re");
    if (!p)
        return std::nullopt;
    std::string out;
    char buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, p)) > 0;)
        out.append(buf, n);
    // Its exit status is lost: atrium's SIGCHLD handler reaps every child.
    pclose(p);
    return out;
}

// Old cliphist keeps no types: a picture is told by its first bytes.
std::string sniff_mime(std::string_view d) {
    if (d.starts_with("\x89PNG\r\n\x1a\n"))
        return "image/png";
    if (d.starts_with("\xff\xd8\xff"))
        return "image/jpeg";
    if (d.starts_with("GIF87a") || d.starts_with("GIF89a"))
        return "image/gif";
    if (d.size() > 12 && d.starts_with("RIFF") && d.substr(8, 4) == "WEBP")
        return "image/webp";
    if (d.starts_with("BM"))
        return "image/bmp";
    return "text/plain;charset=utf-8";
}

} // namespace

// What cliphist kept, brought over once, oldest first so the newest ends on
// top, with cliphist's own decode (its database is bbolt's).
void ClipboardHistory::import_cliphist(std::filesystem::path db) {
    import_done_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (import_done_ < 0)
        return;
    import_source_ = wl_event_loop_add_fd(server_.loop, import_done_, WL_EVENT_READABLE, [](int fd, uint32_t, void* d) {
        uint64_t n;
        [[maybe_unused]] ssize_t r = read(fd, &n, sizeof n);
        static_cast<ClipboardHistory*>(d)->finish_import();
        return 0;
    }, this);
    wlr_log(WLR_INFO, "clipboard: bringing over cliphist's history");
    import_ = std::thread([this, db = std::move(db)] {
        std::vector<std::pair<std::string, std::string>> out;
        const std::string flag = " -db-path '" + db.string() + "'";
        if (auto list = run("cliphist" + flag + " list 2>/dev/null")) {
            std::vector<std::string> ids;
            std::istringstream lines(*list);
            for (std::string line; std::getline(lines, line);)
                if (const auto tab = line.find('\t'); tab != std::string::npos && tab > 0 &&
                    line.find_first_not_of("0123456789") == tab)
                    ids.push_back(line.substr(0, tab));
            for (auto it = ids.rbegin(); it != ids.rend(); ++it)
                if (auto data = run("cliphist" + flag + " decode " + *it + " 2>/dev/null"); data && !data->empty())
                    out.emplace_back(sniff_mime(*data), std::move(*data));
        }
        {
            std::lock_guard lock(import_mutex_);
            imported_ = std::move(out);
        }
        const uint64_t one = 1;
        [[maybe_unused]] ssize_t w = ::write(import_done_, &one, sizeof one);
    });
}

void ClipboardHistory::finish_import() {
    if (import_.joinable())
        import_.join();
    std::vector<std::pair<std::string, std::string>> entries;
    {
        std::lock_guard lock(import_mutex_);
        entries = std::move(imported_);
    }
    size_t n = 0;
    for (auto& [mime, data] : entries)
        n += add(mime, std::move(data)) ? 1 : 0;
    wlr_log(WLR_INFO, "clipboard: brought over %zu of cliphist's %zu entries", n, entries.size());
    if (import_source_) {
        wl_event_source_remove(import_source_);
        import_source_ = nullptr;
    }
}

} // namespace atrium
