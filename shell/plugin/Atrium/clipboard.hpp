#pragma once
// Clipboard history for the Super+V picker, as atrium keeps it (the
// compositor sees every copy: src/clipboard_history.hpp). Each entry's data
// is a file of its own; pictures show from it directly.

#include <QObject>
#include <QVariant>

#include <vector>

namespace atrium {

class ClipboardHistory : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)  // newest first, filtered
    Q_PROPERTY(QVariantMap preview READ preview NOTIFY previewChanged)     // { id, text | image }
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)      // atrium keeps a history

public:
    explicit ClipboardHistory(QObject* parent = nullptr);

    QString query() const { return query_; }
    void setQuery(const QString& query);
    QVariantList results() const { return results_; }
    QVariantMap preview() const { return preview_; }
    bool available() const { return available_; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void showPreview(const QString& id);
    Q_INVOKABLE void copy(const QString& id);
    Q_INVOKABLE void remove(const QString& id);
    Q_INVOKABLE void clear();

signals:
    void queryChanged();
    void resultsChanged();
    void previewChanged();
    void availableChanged();

private:
    struct Entry {
        QString id;
        QString text;   // a one-line preview
        bool image = false;
        QString format; // png, jpeg...
        int width = 0, height = 0;
        QString size;   // "16 KiB"
        QString file;
    };

    void take(const QVariantList& list);
    void filter();
    QVariantMap toMap(const Entry& e) const;
    const Entry* find(const QString& id) const;

    QString query_;
    std::vector<Entry> entries_;
    QVariantList results_;
    QVariantMap preview_;
    bool available_ = true;
};

} // namespace atrium
