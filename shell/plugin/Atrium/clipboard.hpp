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
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)  // pins, then newest first; filtered
    // The type filter (Tinycast's): "" for all, or text, image, file, color,
    // link, email. `filters` are its choices: { value, label }.
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(QVariantList filters READ filters CONSTANT)
    // What an empty list says under the filter.
    Q_PROPERTY(QString emptyText READ emptyText NOTIFY filterChanged)
    // Where the selection lands: past the pins on the newest, when nothing is typed.
    Q_PROPERTY(int landing READ landing NOTIFY resultsChanged)
    Q_PROPERTY(QVariantMap preview READ preview NOTIFY previewChanged)     // { id, text | image }
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)      // atrium keeps a history

public:
    explicit ClipboardHistory(QObject* parent = nullptr);

    QString query() const { return query_; }
    void setQuery(const QString& query);
    QVariantList results() const { return results_; }
    QVariantMap preview() const { return preview_; }
    bool available() const { return available_; }
    QString filter() const { return filter_; }
    void setFilter(const QString& filter);
    QVariantList filters() const;
    QString emptyText() const;
    int landing() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void showPreview(const QString& id);
    Q_INVOKABLE void copy(const QString& id);
    Q_INVOKABLE void remove(const QString& id);
    Q_INVOKABLE void clear();  // all but the pins
    // Kept at the top, past the limit and Clear All; or let go of.
    Q_INVOKABLE void togglePin(const QString& id);
    // The id of the nth pin the list shows (Ctrl+1..0), or "".
    Q_INVOKABLE QString pinAt(int n) const;
    // Where an entry is listed now, or -1.
    Q_INVOKABLE int indexOf(const QString& id) const;

signals:
    void queryChanged();
    void filterChanged();
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
        bool pinned = false;
        QString kind;   // text, image, file, color, link, email
        QString color;  // a colour's #rrggbb, for its swatch
    };

    void take(const QVariantList& list);
    void apply();  // the query and the filter, into results
    QVariantMap toMap(const Entry& e) const;
    const Entry* find(const QString& id) const;

    QString query_;
    QString filter_;
    std::vector<Entry> entries_;
    QVariantList results_;
    QVariantMap preview_;
    bool available_ = true;
};

} // namespace atrium
