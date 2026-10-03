#include "clipboard_history.hpp"

#include "ipc.hpp"
#include "protocols.hpp"
#include "server.hpp"
#include "wl/data_device.hpp"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace atrium {

namespace {

constexpr size_t kTextLimit = 4u << 20;    // a copy bigger than this isn't kept
constexpr size_t kImageLimit = 32u << 20;
constexpr int kReadTimeoutMs = 3000;

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

} // namespace

// atrium's own selection: a history entry, or a copy whose app quit.
struct ClipboardHistory::Source final : wl::DataSource {
    ClipboardHistory& history;
    std::shared_ptr<const std::string> data;
    Source(ClipboardHistory& h, const std::string& mime, std::shared_ptr<const std::string> d)
        : history(h), data(std::move(d)) {
        mime_types_ = clipboard_is_text(mime) ? clipboard_text_mimes() : std::vector<std::string>{mime};
    }
    void send(const std::string&, int fd) override { history.write(fd, data); }
};

// A selection being read from its app.
struct ClipboardHistory::Read {
    wl::DataSource* source = nullptr;
    std::string mime, data;
    size_t limit = 0;
    int fd = -1;
    wl_event_source* readable = nullptr;
    wl_event_source* timeout = nullptr;
    wl::Connection gone;
    ~Read() {
        if (readable)
            wl_event_source_remove(readable);
        if (timeout)
            wl_event_source_remove(timeout);
        if (fd >= 0)
            close(fd);
    }
};

// A copy being handed to an app that pastes it.
struct ClipboardHistory::Write {
    ClipboardHistory* history = nullptr;
    int fd = -1;
    std::shared_ptr<const std::string> data;
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
    std::filesystem::create_directories(dir_, ec);
    std::filesystem::permissions(dir_, std::filesystem::perms::owner_all, ec);
    if (auto text = read_file(dir_ / "index.json", 64u << 20))
        index_ = ClipboardIndex::from_json(*text);
    // Entries whose file is gone are gone.
    std::erase_if(index_.entries(), [&](const ClipboardEntry& e) { return !std::filesystem::exists(file(e), ec); });
    selection_ = server_.wl->data->slot().changed.connect([this](wl::DataSource* s) { selection_changed(s); });
}

ClipboardHistory::~ClipboardHistory() {
    if (restore_)
        wl_event_source_remove(restore_);
    if (reap_)
        wl_event_source_remove(reap_);
    selection_.disconnect();
    recorded_gone_.disconnect();
    read_.reset();
    writes_.clear();
    if (own_ && server_.wl->data->selection() == own_.get())
        server_.wl->data->set_selection(nullptr);
    own_.reset();
}

void ClipboardHistory::selection_changed(wl::DataSource* source) {
    if (source == own_.get() && source)
        return;
    read_.reset();
    if (!source) {
        // Emptied: if that's because its app quit, the copy stays (on the
        // next turn, once the app's going has been seen).
        if (!restore_)
            restore_ = wl_event_loop_add_idle(server_.loop, [](void* d) {
                auto* self = static_cast<ClipboardHistory*>(d);
                self->restore_ = nullptr;
                if (!self->server_.wl->data->selection() && self->source_gone_ && !self->last_id_.empty()) {
                    self->source_gone_ = false;
                    self->copy(self->last_id_);
                }
            }, this);
        return;
    }
    source_gone_ = false;
    recorded_from_ = nullptr;
    recorded_gone_.disconnect();
    if (!server_.config.clipboard_history)
        return;
    if (auto mime = clipboard_mime(source->mime_types()))
        record(source, *mime);
}

void ClipboardHistory::record(wl::DataSource* source, const std::string& mime) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
        return;
    auto r = std::make_unique<Read>();
    r->source = source;
    r->mime = mime;
    r->limit = clipboard_is_text(mime) ? kTextLimit : kImageLimit;
    r->fd = fds[0];
    r->gone = source->events.destroy.connect([this] {
        // The app went before it finished writing: whatever came, if anything.
        if (read_)
            read_->source = nullptr;
    });
    r->readable = wl_event_loop_add_fd(server_.loop, fds[0], WL_EVENT_READABLE, [](int fd, uint32_t mask, void* d) {
        auto* self = static_cast<ClipboardHistory*>(d);
        Read& r = *self->read_;
        char buf[65536];
        for (;;) {
            const ssize_t n = read(fd, buf, sizeof buf);
            if (n > 0) {
                if (r.data.size() + size_t(n) > r.limit) {
                    self->finish_read(false);  // too big to keep
                    return 0;
                }
                r.data.append(buf, size_t(n));
                continue;
            }
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                self->finish_read(n == 0);
                return 0;
            }
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
            self->finish_read(true);
        return 0;
    }, this);
    r->timeout = wl_event_loop_add_timer(server_.loop, [](void* d) {
        static_cast<ClipboardHistory*>(d)->finish_read(false);
        return 0;
    }, this);
    wl_event_source_timer_update(r->timeout, kReadTimeoutMs);
    read_ = std::move(r);
    source->send(mime, fds[1]);  // it closes the write end
}

void ClipboardHistory::finish_read(bool ok) {
    if (!read_)
        return;
    std::unique_ptr<Read> r = std::move(read_);
    if (!ok || r->data.empty())
        return;
    wl::DataSource* from = r->source;
    store(r->mime, std::move(r->data), true);
    if (from && server_.wl->data->selection() == from) {
        recorded_from_ = from;
        recorded_gone_ = from->events.destroy.connect([this] {
            source_gone_ = true;
            recorded_from_ = nullptr;
            recorded_gone_.disconnect();
        });
    }
}

bool ClipboardHistory::add(const std::string& mime, std::string data) {
    if (!server_.config.clipboard_history || data.empty() ||
        data.size() > (clipboard_is_text(mime) ? kTextLimit : kImageLimit))
        return false;
    store(mime, std::move(data), false);
    return true;
}

void ClipboardHistory::store(const std::string& mime, std::string data, bool current) {
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
    if (current)
        last_id_ = e.id;
    save_index();
    changed();
}

bool ClipboardHistory::copy(const std::string& id) {
    const ClipboardEntry* e = index_.find(id);
    if (!e)
        return false;
    auto data = read_file(file(*e), kImageLimit);
    if (!data)
        return false;
    const std::string mime = e->mime;
    index_.to_front(id);
    last_id_ = id;
    save_index();
    offer(mime, std::make_shared<const std::string>(std::move(*data)));
    changed();
    return true;
}

bool ClipboardHistory::set_text(const std::string& text) {
    if (text.size() > kTextLimit)
        return false;
    if (server_.config.clipboard_history && !text.empty())
        store("text/plain;charset=utf-8", text, true);
    offer("text/plain;charset=utf-8", std::make_shared<const std::string>(text));
    return true;
}

bool ClipboardHistory::set_file(const std::string& mime, const std::filesystem::path& path) {
    if (mime.empty() || mime.find('/') == std::string::npos)
        return false;
    auto data = read_file(path, kImageLimit);
    if (!data)
        return false;
    auto shared = std::make_shared<const std::string>(*data);
    if (server_.config.clipboard_history && clipboard_mime({mime}))
        store(mime, std::move(*data), true);
    offer(mime, shared);
    return true;
}

bool ClipboardHistory::add_file(const std::string& mime, const std::filesystem::path& path) {
    if (!clipboard_mime({mime}))
        return false;
    auto data = read_file(path, kImageLimit);
    return data && add(mime, std::move(*data));
}

void ClipboardHistory::offer(const std::string& mime, std::shared_ptr<const std::string> data) {
    // Ours before it's the selection (so it isn't recorded as a new copy);
    // the old one goes once it no longer is.
    std::unique_ptr<Source> old = std::move(own_);
    own_ = std::make_unique<Source>(*this, mime, std::move(data));
    server_.wl->data->set_selection(own_.get());
    old.reset();
}

void ClipboardHistory::write(int fd, std::shared_ptr<const std::string> data) {
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
    if (last_id_ == id)
        last_id_.clear();
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
    last_id_.clear();
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

} // namespace atrium
