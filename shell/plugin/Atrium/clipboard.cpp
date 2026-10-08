#include "clipboard.hpp"

#include "compositor.hpp"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QLocale>
#include <QTemporaryFile>
#include <QUrl>

namespace atrium {

namespace {

constexpr qint64 kPreviewBytes = 256 * 1024;  // of a text, for the preview pane

} // namespace

ClipboardHistory::ClipboardHistory(QObject* parent) : QObject(parent) {
    connect(Compositor::instance(), &Compositor::clipboardChanged, this, &ClipboardHistory::refresh);
}

void ClipboardHistory::setQuery(const QString& query) {
    if (query == query_)
        return;
    query_ = query;
    emit queryChanged();
    filter();
}

void ClipboardHistory::refresh() {
    Compositor::instance()->clipboardHistory([this](bool ok, const QVariantList& list) {
        if (ok != available_) {
            available_ = ok;
            emit availableChanged();
        }
        take(list);
    });
}

void ClipboardHistory::take(const QVariantList& list) {
    std::vector<Entry> fresh;
    for (const QVariant& v : list) {
        const QVariantMap m = v.toMap();
        Entry e;
        e.id = m.value("id").toString();
        e.file = m.value("file").toString();
        const QString mime = m.value("mime").toString();
        e.image = mime.startsWith("image/");
        e.size = QLocale().formattedDataSize(m.value("size").toLongLong(), 0, QLocale::DataSizeIecFormat);
        if (e.image) {
            e.format = mime.mid(6);
            // Its header only: cheap.
            const QSize size = QImageReader(e.file).size();
            e.width = size.width();
            e.height = size.height();
        } else {
            e.text = m.value("preview").toString();
        }
        fresh.push_back(std::move(e));
    }
    entries_ = std::move(fresh);
    filter();
}

const ClipboardHistory::Entry* ClipboardHistory::find(const QString& id) const {
    for (const Entry& e : entries_)
        if (e.id == id)
            return &e;
    return nullptr;
}

QVariantMap ClipboardHistory::toMap(const Entry& e) const {
    return {{"id", e.id}, {"text", e.text}, {"image", e.image}, {"width", e.width},
            {"height", e.height}, {"size", e.size}, {"format", e.format},
            {"thumb", e.image ? QUrl::fromLocalFile(e.file).toString() : QString()}};
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
    preview_ = toMap(*e);
    // The list shows a line of it; the preview all of it (or a lot).
    if (!e->image)
        if (QFile f(e->file); f.open(QIODevice::ReadOnly))
            preview_["full"] = QString::fromUtf8(f.read(kPreviewBytes));
    emit previewChanged();
}

void ClipboardHistory::copy(const QString& id) {
    const Entry* e = find(id);
    if (!e)
        return;
    Compositor* c = Compositor::instance();
    // Most apps paste pictures as PNG only (a phone's screenshots are JPEG):
    // other pictures go back as PNG, in their place in the list.
    if (e->image && e->format != "png") {
        const QImage image(e->file);
        auto* png = new QTemporaryFile(QDir::tempPath() + "/atrium-clip-XXXXXX.png", this);
        if (!image.isNull() && png->open() && image.save(png, "PNG")) {
            png->close();
            // Gone once atrium has read it.
            c->clipboard("set", {{"mime", "image/png"}, {"path", png->fileName()}}, [png](bool) { png->deleteLater(); });
            c->clipboard("delete", {{"entry", id}});
            return;
        }
        delete png;
    }
    c->clipboard("copy", {{"entry", id}});
}

void ClipboardHistory::remove(const QString& id) {
    Compositor::instance()->clipboard("delete", {{"entry", id}});
    std::erase_if(entries_, [&id](const Entry& x) { return x.id == id; });
    filter();
}

void ClipboardHistory::clear() {
    Compositor::instance()->clipboard("clear");
    entries_.clear();
    preview_.clear();
    emit previewChanged();
    filter();
}

} // namespace atrium
