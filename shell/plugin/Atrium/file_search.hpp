#pragma once
// Search Files, the palette's screen: file and folder names under the
// folders Settings names (files.folders), found through plocate's database
// when there is one and by walking the folders when not; with nothing
// typed, the recently used files desktop apps record. Names only, no index
// of atrium's own. `FileSearch { query: field.text }`.

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariant>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace atrium {

class FileSearch : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    // all, folders, documents, images, audio, videos, archives
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    // [{ path, name, parent, folder, icon, mime }], best first
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)
    // The results are the recently used files (nothing typed).
    Q_PROPERTY(bool recent READ recent NOTIFY resultsChanged)

public:
    explicit FileSearch(QObject* parent = nullptr);
    ~FileSearch() override;

    QString query() const { return query_; }
    void setQuery(const QString& query);
    QString filter() const { return filter_; }
    void setFilter(const QString& filter);
    QVariantList results() const { return results_; }
    bool searching() const { return searching_; }
    bool recent() const { return recent_; }

    // [{ value, label }] for the type menu.
    Q_INVOKABLE QVariantList filters() const;
    // What the preview shows: { name, where, type, size, modified, created,
    // image (a file URL, for pictures), text (the start of a text file) }.
    Q_INVOKABLE QVariantMap details(const QString& path) const;
    Q_INVOKABLE void open(const QString& path) const;
    Q_INVOKABLE void reveal(const QString& path) const;
    Q_INVOKABLE void copyFile(const QString& path) const;
    Q_INVOKABLE void copyText(const QString& text) const;
    // To the trash; the row leaves the results. False when it couldn't.
    Q_INVOKABLE bool trash(const QString& path);

signals:
    void queryChanged();
    void filterChanged();
    void resultsChanged();
    void searchingChanged();

private:
    struct Hit {
        std::string path;
        int score = 0;
    };

    void run();
    void runRecent();
    void runLocate(const std::vector<std::string>& terms);
    void runWalk(const std::vector<std::string>& terms);
    void publish(std::vector<Hit> hits, bool recent);
    void setSearching(bool on);
    std::vector<std::string> roots() const;

    QString query_;
    QString filter_ = QStringLiteral("all");
    QVariantList results_;
    bool searching_ = false;
    bool recent_ = true;
    QTimer debounce_;
    // Each search's number: a slower, older one never publishes over a newer.
    std::shared_ptr<std::atomic<int>> generation_ = std::make_shared<std::atomic<int>>(0);
    std::unique_ptr<QProcess> locate_;
    QByteArray locateBuffer_;
    std::vector<std::string> candidates_;
    bool locateUsable_ = true;
};

} // namespace atrium
