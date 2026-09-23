#include "clipboard.hpp"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

namespace atrium {

namespace {

constexpr int kThumbnails = 80;  // the most recent pictures get one

// "[[ binary data 16 KiB png 804x147 ]]"
const QRegularExpression kBinary(R"(^\[\[ binary data (\S+ \S+) (\w+) (\d+)x(\d+) \]\]$)");

} // namespace

ClipboardHistory::ClipboardHistory(QObject* parent) : QObject(parent) {
    cliphist_ = QStandardPaths::findExecutable("cliphist");
    wlcopy_ = QStandardPaths::findExecutable("wl-copy");
    QString cache = qEnvironmentVariable("XDG_CACHE_HOME");
    if (cache.isEmpty())
        cache = QDir::homePath() + "/.cache";
    cacheDir_ = cache + "/atrium/clipboard";
    QDir().mkpath(cacheDir_);
}

void ClipboardHistory::setQuery(const QString& query) {
    if (query == query_)
        return;
    query_ = query;
    emit queryChanged();
    filter();
}

void ClipboardHistory::refresh() {
    if (!available())
        return;
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p] {
        std::vector<Entry> fresh;
        const QList<QByteArray> lines = p->readAllStandardOutput().split('\n');
        for (const QByteArray& raw : lines) {
            const qsizetype tab = raw.indexOf('\t');
            if (tab <= 0)
                continue;
            Entry e;
            e.id = QString::fromUtf8(raw.left(tab));
            e.text = QString::fromUtf8(raw.mid(tab + 1));
            if (const auto m = kBinary.match(e.text); m.hasMatch()) {
                e.image = true;
                e.size = m.captured(1);
                e.format = m.captured(2);
                e.width = m.captured(3).toInt();
                e.height = m.captured(4).toInt();
                if (QFile::exists(cacheFile(e)))
                    e.thumb = QUrl::fromLocalFile(cacheFile(e)).toString();
            }
            fresh.push_back(std::move(e));
        }
        entries_ = std::move(fresh);
        // Thumbnails for the recent pictures that have none yet.
        decodeQueue_.clear();
        int pictures = 0;
        for (const Entry& e : entries_)
            if (e.image && pictures++ < kThumbnails && e.thumb.isEmpty())
                decodeQueue_.push_back(e.id);
        filter();
        decodeNext();
        p->deleteLater();
    });
    p->start(cliphist_, {"list"});
}

QString ClipboardHistory::cacheFile(const Entry& e) const {
    return cacheDir_ + "/" + e.id + "." + (e.format.isEmpty() ? "png" : e.format);
}

const ClipboardHistory::Entry* ClipboardHistory::find(const QString& id) const {
    for (const Entry& e : entries_)
        if (e.id == id)
            return &e;
    return nullptr;
}

QByteArray ClipboardHistory::lineFor(const Entry& e) const {
    return (e.id + "\t" + e.text + "\n").toUtf8();
}

// One picture at a time, so a long history never floods the machine.
void ClipboardHistory::decodeNext() {
    if (decoding_ || decodeQueue_.empty())
        return;
    const QString id = decodeQueue_.front();
    decodeQueue_.pop_front();
    const Entry* e = find(id);
    if (!e) {
        decodeNext();
        return;
    }
    decoding_ = true;
    const QString file = cacheFile(*e);
    auto* p = new QProcess(this);
    p->setStandardOutputFile(file);
    connect(p, &QProcess::finished, this, [this, p, id, file](int code) {
        decoding_ = false;
        for (Entry& e : entries_)
            if (e.id == id && code == 0)
                e.thumb = QUrl::fromLocalFile(file).toString();
        if (code != 0)
            QFile::remove(file);
        filter();
        decodeNext();
        p->deleteLater();
    });
    p->start(cliphist_, {"decode", id});
}

QVariantMap ClipboardHistory::toMap(const Entry& e) const {
    return {{"id", e.id}, {"text", e.text}, {"image", e.image}, {"width", e.width},
            {"height", e.height}, {"size", e.size}, {"format", e.format}, {"thumb", e.thumb}};
}

void ClipboardHistory::filter() {
    const QString q = query_.trimmed();
    QVariantList out;
    for (const Entry& e : entries_) {
        if (!q.isEmpty()) {
            // Pictures match by kind ("png", "image"), text by what it says.
            const bool hit = e.image ? (e.format.contains(q, Qt::CaseInsensitive) ||
                                        QStringLiteral("image").contains(q, Qt::CaseInsensitive))
                                     : e.text.contains(q, Qt::CaseInsensitive);
            if (!hit)
                continue;
        }
        out.push_back(toMap(e));
    }
    results_ = out;
    emit resultsChanged();
}

void ClipboardHistory::showPreview(const QString& id) {
    const Entry* e = find(id);
    if (!e) {
        preview_.clear();
        emit previewChanged();
        return;
    }
    if (e->image) {
        preview_ = toMap(*e);
        emit previewChanged();
        return;
    }
    // cliphist lists a shortened line; the preview shows all of it.
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p, id] {
        if (const Entry* e = find(id)) {
            QVariantMap m = toMap(*e);
            m["full"] = QString::fromUtf8(p->readAllStandardOutput());
            preview_ = m;
            emit previewChanged();
        }
        p->deleteLater();
    });
    p->start(cliphist_, {"decode", id});
}

void ClipboardHistory::copy(const QString& id) {
    const Entry* e = find(id);
    if (!e || wlcopy_.isEmpty())
        return;
    // cliphist decode | wl-copy, with the picture's type for images.
    auto* decode = new QProcess(this);
    auto* copy = new QProcess(this);
    decode->setStandardOutputProcess(copy);
    QStringList args;
    if (e->image)
        args << "--type" << ("image/" + e->format);
    connect(copy, &QProcess::finished, copy, &QObject::deleteLater);
    connect(decode, &QProcess::finished, decode, &QObject::deleteLater);
    copy->start(wlcopy_, args);
    decode->start(cliphist_, {"decode", id});
}

void ClipboardHistory::remove(const QString& id) {
    const Entry* e = find(id);
    if (!e)
        return;
    QFile::remove(cacheFile(*e));
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, p, &QObject::deleteLater);
    p->start(cliphist_, {"delete"});
    p->write(lineFor(*e));
    p->closeWriteChannel();
    std::erase_if(entries_, [&id](const Entry& x) { return x.id == id; });
    filter();
}

void ClipboardHistory::clear() {
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, p, &QObject::deleteLater);
    p->start(cliphist_, {"wipe"});
    QDir(cacheDir_).removeRecursively();
    QDir().mkpath(cacheDir_);
    entries_.clear();
    decodeQueue_.clear();
    preview_.clear();
    emit previewChanged();
    filter();
}

} // namespace atrium
