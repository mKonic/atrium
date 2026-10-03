#include "history.hpp"

#include "clipboard_core.hpp"
#include "compositor.hpp"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryFile>

namespace atrium::clipsync {

namespace {

// Kept beyond the exchanged few: a clip too big for the phone doesn't cost
// it one of its places.
constexpr std::size_t kKept = 2 * kHistory;

// Where atrium keeps the clipboard's history (Server::start_clipboard_history).
QString historyDir() {
    if (const QString own = qEnvironmentVariable("ATRIUM_CLIPBOARD_DIR"); !own.isEmpty())
        return own;
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty())
        state = QDir::homePath() + "/.local/state";
    return state + "/atrium/clipboard";
}

} // namespace

RecentClips::RecentClips() {
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty())
        state = QDir::homePath() + "/.local/state";
    QDir().mkpath(state + "/atrium/clipsync");
    file_ = state + "/atrium/clipsync/history";

    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly)) {
        seed();
        return;
    }
    Reader r(kMaxClip);
    r.feed(f.readAll().toStdString());
    while (auto m = r.next())
        if (m->type == Type::Clip && clips_.size() < kKept)
            clips_.push_back(std::move(m->clip));
}

void RecentClips::seed() {
    const QString dir = historyDir();
    QFile index(dir + "/index.json");
    if (!index.open(QIODevice::ReadOnly))
        return;
    const ClipboardIndex entries = ClipboardIndex::from_json(index.readAll().toStdString());
    for (const ClipboardEntry& e : entries.entries()) {
        if (clips_.size() == std::size_t(kHistory))
            break;
        if (e.size == 0 || e.size > kMaxClip)
            continue;
        QFile f(dir + "/" + QString::fromStdString(e.id));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray data = f.readAll();
        const std::string mime = clipboard_is_text(e.mime) ? std::string(kText) : e.mime;
        clips_.push_back({mime, data.toStdString(), 0});
    }
    save();
}

void RecentClips::current(const Clip& c) {
    const std::uint64_t h = c.hash();
    Clip keep = c;
    for (auto it = clips_.begin(); it != clips_.end(); ++it)
        if (it->hash() == h) {
            // Already the clipboard's: nothing new (a restart seeing it again).
            if (it == clips_.begin() && c.time == 0)
                return;
            if (keep.time == 0)
                keep.time = it->time;
            clips_.erase(it);
            break;
        }
    clips_.insert(clips_.begin(), std::move(keep));
    if (clips_.size() > kKept)
        clips_.resize(kKept);
    save();
}

void RecentClips::older(const Clip& c) {
    const std::uint64_t h = c.hash();
    for (const Clip& x : clips_)
        if (x.hash() == h)
            return;
    auto at = clips_.empty() ? clips_.end() : clips_.begin() + 1;
    while (at != clips_.end() && at->time >= c.time)
        ++at;
    clips_.insert(at, c);
    if (clips_.size() > kKept)
        clips_.resize(kKept);
    save();
}

void RecentClips::toHistory(const Clip& c) const {
    Compositor* atrium = Compositor::instance();
    if (clipboard_is_text(c.mime)) {
        atrium->clipboard("add", {{"text", QString::fromUtf8(c.data.data(), qsizetype(c.data.size()))}});
        return;
    }
    // A picture goes by a file, gone once atrium has read it.
    auto* file = new QTemporaryFile(QDir::tempPath() + "/atrium-clipsync-XXXXXX");
    if (!file->open() || file->write(c.data.data(), qint64(c.data.size())) != qint64(c.data.size())) {
        delete file;
        return;
    }
    file->close();
    atrium->clipboard("add", {{"mime", QString::fromStdString(c.mime)}, {"path", file->fileName()}},
                      [file](bool) { delete file; });
}

void RecentClips::save() const {
    // What was copied: for this user only.
    QSaveFile f(file_);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    for (const Clip& c : clips_) {
        const std::string frame = clipFrame(c, Flags::History);
        f.write(frame.data(), qint64(frame.size()));
    }
    f.commit();
}

} // namespace atrium::clipsync
