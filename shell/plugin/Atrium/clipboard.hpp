#pragma once
// Clipboard history for the Super+V picker, over cliphist (the store the
// session's `wl-paste --watch cliphist store` watchers fill). Everything
// runs asynchronously: listing, decoding pictures for thumbnails into a
// cache, copying back, deleting.

#include <QObject>
#include <QProcess>
#include <QVariant>

#include <deque>

namespace atrium {

class ClipboardHistory : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)  // newest first, filtered
    Q_PROPERTY(QVariantMap preview READ preview NOTIFY previewChanged)     // { id, text | image }
    Q_PROPERTY(bool available READ available CONSTANT)                    // cliphist is installed

public:
    explicit ClipboardHistory(QObject* parent = nullptr);

    QString query() const { return query_; }
    void setQuery(const QString& query);
    QVariantList results() const { return results_; }
    QVariantMap preview() const { return preview_; }
    bool available() const { return !cliphist_.isEmpty(); }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void showPreview(const QString& id);
    Q_INVOKABLE void copy(const QString& id);
    Q_INVOKABLE void remove(const QString& id);
    Q_INVOKABLE void clear();

signals:
    void queryChanged();
    void resultsChanged();
    void previewChanged();

private:
    struct Entry {
        QString id;
        QString text;   // cliphist's one-line preview
        bool image = false;
        QString format; // png, jpeg...
        int width = 0, height = 0;
        QString size;   // "16 KiB"
        QString thumb;  // file URL once decoded
    };

    void filter();
    QVariantMap toMap(const Entry& e) const;
    QString cacheFile(const Entry& e) const;
    void decodeNext();
    const Entry* find(const QString& id) const;
    QByteArray lineFor(const Entry& e) const;

    QString cliphist_, wlcopy_;
    QString query_;
    std::vector<Entry> entries_;
    QVariantList results_;
    QVariantMap preview_;
    std::deque<QString> decodeQueue_;  // pictures waiting for a thumbnail
    bool decoding_ = false;
    QString cacheDir_;
};

} // namespace atrium
