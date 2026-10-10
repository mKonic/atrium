#include "clipboard.hpp"

#include <QCoreApplication>

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

// Tinycast's ClipboardFilter: the value, its menu label, what an empty list says.
struct Filter {
    const char* value;
    const char* label;
    const char* empty;
};
constexpr Filter kFilters[] = {
    {"", QT_TRANSLATE_NOOP("clipboard", "All Types"), QT_TRANSLATE_NOOP("clipboard", "Nothing copied yet")},
    {"text", QT_TRANSLATE_NOOP("clipboard", "Text Only"), QT_TRANSLATE_NOOP("clipboard", "No text in clipboard history")},
    {"image", QT_TRANSLATE_NOOP("clipboard", "Images Only"), QT_TRANSLATE_NOOP("clipboard", "No images in clipboard history")},
    {"file", QT_TRANSLATE_NOOP("clipboard", "Files Only"), QT_TRANSLATE_NOOP("clipboard", "No files in clipboard history")},
    {"color", QT_TRANSLATE_NOOP("clipboard", "Colours Only"), QT_TRANSLATE_NOOP("clipboard", "No colours in clipboard history")},
    {"link", QT_TRANSLATE_NOOP("clipboard", "Links Only"), QT_TRANSLATE_NOOP("clipboard", "No links in clipboard history")},
    {"email", QT_TRANSLATE_NOOP("clipboard", "Emails Only"), QT_TRANSLATE_NOOP("clipboard", "No email addresses in clipboard history")},
};

} // namespace

ClipboardHistory::ClipboardHistory(QObject* parent) : QObject(parent) {
    connect(Compositor::instance(), &Compositor::clipboardChanged, this, &ClipboardHistory::refresh);
}

void ClipboardHistory::setQuery(const QString& query) {
    if (query == query_)
        return;
    query_ = query;
    emit queryChanged();
    apply();
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
        e.pinned = m.value("pinned").toBool();
        e.kind = m.value("kind").toString();
        e.color = m.value("color").toString();
        // CSS writes alpha last (#rrggbbaa), QML first (#aarrggbb).
        if (e.color.size() == 9)
            e.color = "#" + e.color.mid(7) + e.color.mid(1, 6);
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
    apply();
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
            {"thumb", e.image ? QUrl::fromLocalFile(e.file).toString() : QString()}, {"pinned", e.pinned},
            {"kind", e.kind}, {"color", e.color}};
}

void ClipboardHistory::apply() {
    const QString q = query_.trimmed();
    QVariantList out;
    for (const Entry& e : entries_) {
        if (!filter_.isEmpty() && e.kind != filter_)
            continue;
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
    apply();
}

void ClipboardHistory::clear() {
    Compositor::instance()->clipboard("clear");
    std::erase_if(entries_, [](const Entry& e) { return !e.pinned; });
    preview_.clear();
    emit previewChanged();
    apply();
}

void ClipboardHistory::togglePin(const QString& id) {
    const Entry* e = find(id);
    if (!e)
        return;
    // The list comes back in its new order once atrium has it.
    Compositor::instance()->clipboard("pin", {{"entry", id}, {"pinned", !e->pinned}});
}

QString ClipboardHistory::pinAt(int n) const {
    if (n < 0 || n >= results_.size())
        return {};
    const QVariantMap m = results_[n].toMap();
    return m.value("pinned").toBool() ? m.value("id").toString() : QString();
}

int ClipboardHistory::indexOf(const QString& id) const {
    for (qsizetype i = 0; i < results_.size(); ++i)
        if (results_[i].toMap().value("id").toString() == id)
            return int(i);
    return -1;
}

int ClipboardHistory::landing() const {
    if (!query_.trimmed().isEmpty())
        return 0;
    for (qsizetype i = 0; i < results_.size(); ++i)
        if (!results_[i].toMap().value("pinned").toBool())
            return int(i);
    return 0;
}

void ClipboardHistory::setFilter(const QString& filter) {
    if (filter == filter_)
        return;
    filter_ = filter;
    emit filterChanged();
    apply();
}

QVariantList ClipboardHistory::filters() const {
    QVariantList out;
    for (const Filter& f : kFilters)
        out.push_back(QVariantMap{{"value", QString(f.value)}, {"label", QCoreApplication::translate("clipboard", f.label)}});
    return out;
}

QString ClipboardHistory::emptyText() const {
    for (const Filter& f : kFilters)
        if (filter_ == f.value)
            return QCoreApplication::translate("clipboard", f.empty);
    return QCoreApplication::translate("clipboard", kFilters[0].empty);
}

} // namespace atrium
