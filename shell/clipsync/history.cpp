#include "history.hpp"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace atrium::clipsync {

namespace {

// Kept beyond the exchanged few: a clip too big for the phone doesn't cost
// it one of its places.
constexpr std::size_t kKept = 2 * kHistory;

// "[[ binary data 16 KiB png 804x147 ]]"
const QRegularExpression kBinary(R"(^\[\[ binary data \S+ \S+ (\w+) \d+x\d+ \]\]$)");

} // namespace

RecentClips::RecentClips() {
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty())
        state = QDir::homePath() + "/.local/state";
    QDir().mkpath(state + "/atrium/clipsync");
    file_ = state + "/atrium/clipsync/history";
    cliphist_ = QStandardPaths::findExecutable("cliphist");

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
    if (cliphist_.isEmpty())
        return;
    QProcess list;
    list.start(cliphist_, {"list"});
    if (!list.waitForFinished(5000))
        return;
    for (const QByteArray& line : list.readAllStandardOutput().split('\n')) {
        if (clips_.size() == std::size_t(kHistory))
            break;
        const qsizetype tab = line.indexOf('\t');
        if (tab <= 0)
            continue;
        QProcess decode;
        decode.start(cliphist_, {"decode", QString::fromUtf8(line.left(tab))});
        if (!decode.waitForFinished(5000))
            continue;
        const QByteArray data = decode.readAllStandardOutput();
        if (data.isEmpty() || std::size_t(data.size()) > kMaxClip)
            continue;
        const auto m = kBinary.match(QString::fromUtf8(line.mid(tab + 1)));
        const std::string mime = m.hasMatch() ? "image/" + m.captured(1).toStdString() : std::string(kText);
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

void RecentClips::toCliphist(const Clip& c) const {
    if (cliphist_.isEmpty())
        return;
    auto* p = new QProcess;
    QObject::connect(p, &QProcess::finished, p, &QObject::deleteLater);
    p->start(cliphist_, {"store"});
    p->write(c.data.data(), qint64(c.data.size()));
    p->closeWriteChannel();
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
