#include "file_search.hpp"

#include "compositor.hpp"
#include "file_search_core.hpp"
#include "launcher_ranking.hpp"

#include <QClipboard>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QMimeData>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

#include <algorithm>

namespace atrium {

namespace {

constexpr int kShown = 200;       // rows the list holds
constexpr int kRecent = 20;
constexpr int kDebounceMs = 120;  // typing: only the last keystroke searches
constexpr int kPublishMs = 150;   // a long search shows its best so far this often

std::vector<std::string> ignorePatterns() {
    std::vector<std::string> patterns = file_search::IgnoreList::defaults();
    for (const QVariant& v : Compositor::instance()->setting("files.ignore", QVariantList()).toList())
        patterns.push_back(v.toString().toStdString());
    return patterns;
}

QString sizeText(qint64 bytes) {
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

} // namespace

FileSearch::FileSearch(QObject* parent) : QObject(parent) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(kDebounceMs);
    connect(&debounce_, &QTimer::timeout, this, &FileSearch::run);
    locateUsable_ = !QStandardPaths::findExecutable("plocate").isEmpty();
    runRecent();
}

FileSearch::~FileSearch() {
    ++*generation_;
    if (locate_) {
        locate_->disconnect(this);
        locate_->kill();
        locate_->waitForFinished(200);
    }
}

void FileSearch::setQuery(const QString& query) {
    if (query == query_)
        return;
    query_ = query;
    emit queryChanged();
    // An empty field has no next keystroke to wait for.
    if (query_.trimmed().isEmpty()) {
        debounce_.stop();
        run();
    } else {
        debounce_.start();
    }
}

void FileSearch::setFilter(const QString& filter) {
    if (filter == filter_)
        return;
    filter_ = filter;
    emit filterChanged();
    run();  // the same words, asked again: a filter narrows the search, not the 200 rows it left
}

QVariantList FileSearch::filters() const {
    return {QVariantMap{{"value", "all"}, {"label", "All Types"}}, QVariantMap{{"value", "folders"}, {"label", "Folders"}},
            QVariantMap{{"value", "documents"}, {"label", "Documents"}}, QVariantMap{{"value", "images"}, {"label", "Images"}},
            QVariantMap{{"value", "audio"}, {"label", "Audio"}}, QVariantMap{{"value", "videos"}, {"label", "Videos"}},
            QVariantMap{{"value", "archives"}, {"label", "Archives"}}};
}

std::vector<std::string> FileSearch::roots() const {
    std::vector<std::string> out;
    for (const QVariant& v : Compositor::instance()->setting("files.folders", QVariantList{"~"}).toList()) {
        QString p = v.toString().trimmed();
        if (p == "~" || p.startsWith("~/"))
            p.replace(0, 1, QDir::homePath());
        p = QDir::cleanPath(p);
        if (!p.isEmpty() && QFileInfo(p).isDir())
            out.push_back(p.toStdString());
    }
    return out;
}

void FileSearch::setSearching(bool on) {
    if (on != searching_) {
        searching_ = on;
        emit searchingChanged();
    }
}

void FileSearch::run() {
    ++*generation_;
    // Maybe called from the old search's own signal: gone once it returns.
    if (locate_) {
        QProcess* old = locate_.release();
        old->disconnect(this);
        old->kill();
        old->deleteLater();
    }
    candidates_.clear();
    const std::vector<std::string> t = file_search::terms(query_.toStdString());
    if (t.empty()) {
        runRecent();
        return;
    }
    setSearching(true);
    if (locateUsable_)
        runLocate(t);
    else
        runWalk(t);
}

void FileSearch::runRecent() {
    QString data = qEnvironmentVariable("XDG_DATA_HOME");
    if (data.isEmpty())
        data = QDir::homePath() + "/.local/share";
    QFile f(data + "/recently-used.xbel");
    std::vector<Hit> hits;
    if (f.open(QIODevice::ReadOnly)) {
        const file_search::IgnoreList ignore(ignorePatterns());
        const std::vector<std::string> r = roots();
        for (const file_search::Recent& rec : file_search::parse_recent(f.readAll().toStdString())) {
            if (!file_search::admitted(rec.path, r, ignore) || !QFileInfo::exists(QString::fromStdString(rec.path)))
                continue;
            hits.push_back({rec.path, 0});
            if (hits.size() >= size_t(kRecent) * 3)  // the type filter may drop some
                break;
        }
    }
    setSearching(false);
    publish(std::move(hits), true);
}

// plocate's database: every file name on the machine, read as it streams in;
// the best so far shows while it does.
void FileSearch::runLocate(const std::vector<std::string>& terms) {
    locate_ = std::make_unique<QProcess>();
    QStringList args{"-i", "-b", "-0"};
    for (const std::string& t : terms)
        args << QString::fromStdString(t);
    locate_->setProgram("plocate");
    locate_->setArguments(args);
    locateBuffer_.clear();
    const std::vector<std::string> r = roots();
    auto ignore = std::make_shared<file_search::IgnoreList>(ignorePatterns());
    QProcess* p = locate_.get();
    auto* publisher = new QTimer(p);
    publisher->setInterval(kPublishMs);
    connect(publisher, &QTimer::timeout, this, [this] {
        std::vector<Hit> hits;
        for (const std::string& c : candidates_)
            hits.push_back({c, 0});
        publish(std::move(hits), false);
    });
    connect(p, &QProcess::readyReadStandardOutput, this, [this, p, r, ignore, terms] {
        locateBuffer_ += p->readAllStandardOutput();
        qsizetype start = 0, zero;
        while ((zero = locateBuffer_.indexOf('\0', start)) >= 0) {
            const std::string path = locateBuffer_.mid(start, zero - start).toStdString();
            start = zero + 1;
            const size_t slash = path.rfind('/');
            // -b matched the words anywhere in the name; the terms check keeps it honest.
            if (file_search::admitted(path, r, *ignore) &&
                file_search::name_matches(std::string_view(path).substr(slash + 1), terms))
                candidates_.push_back(path);
        }
        locateBuffer_.remove(0, start);
    });
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
        const QByteArray err = p->readAllStandardError();
        // No database (never built, or unreadable: "Permission denied" exits
        // like "nothing found" does, but says so): walk the folders instead.
        if (code != 0 && candidates_.empty() && !err.trimmed().isEmpty()) {
            qWarning("atrium: plocate failed (%s); Search Files walks its folders", err.trimmed().constData());
            locateUsable_ = false;
            run();
            return;
        }
        std::vector<Hit> hits;
        for (const std::string& c : candidates_)
            hits.push_back({c, 0});
        setSearching(false);
        publish(std::move(hits), false);
        locate_.release()->deleteLater();
    });
    connect(p, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            locateUsable_ = false;
            run();
        }
    });
    publisher->start();
    p->start();
}

// No database: the folders themselves, walked on a thread of its own.
void FileSearch::runWalk(const std::vector<std::string>& terms) {
    const int gen = *generation_;
    auto generation = generation_;
    const std::vector<std::string> r = roots();
    auto ignore = std::make_shared<file_search::IgnoreList>(ignorePatterns());
    QThread* worker = QThread::create([this, gen, generation, r, ignore, terms] {
        std::vector<std::string> found;
        qint64 lastPublish = QDateTime::currentMSecsSinceEpoch();
        auto post = [&](bool done) {
            QMetaObject::invokeMethod(this, [this, gen, generation, found, done] {
                if (*generation != gen)
                    return;
                candidates_ = found;
                std::vector<Hit> hits;
                for (const std::string& c : candidates_)
                    hits.push_back({c, 0});
                if (done)
                    setSearching(false);
                publish(std::move(hits), false);
            });
        };
        for (const std::string& root : r) {
            QDirIterator it(QString::fromStdString(root), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                if (*generation != gen)
                    return;
                const std::string path = it.next().toStdString();
                if (!file_search::admitted(path, r, *ignore))
                    continue;
                if (file_search::name_matches(it.fileName().toStdString(), terms))
                    found.push_back(path);
                if (QDateTime::currentMSecsSinceEpoch() - lastPublish > kPublishMs) {
                    post(false);
                    lastPublish = QDateTime::currentMSecsSinceEpoch();
                }
            }
        }
        post(true);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);
}

void FileSearch::publish(std::vector<Hit> hits, bool recent) {
    static const QMimeDatabase mimes;
    const std::u32string q = foldText(query_);
    // Best names first: the palette's own scorer over the file name.
    if (!recent) {
        for (Hit& h : hits) {
            const size_t slash = h.path.rfind('/');
            const QString name = QString::fromStdString(h.path.substr(slash + 1));
            const rank::Text text = prepareText(name);
            h.score = rank::score(q, text).value_or(0);
            // The name itself, or all of it but the extension: above any longer one.
            const std::u32string stem = foldText(name.section('.', 0, 0));
            if (text.folded == q)
                h.score += 200;
            else if (stem == q)
                h.score += 100;
        }
        const size_t keep = std::min(hits.size(), size_t(kShown) * 4);
        std::partial_sort(hits.begin(), hits.begin() + ptrdiff_t(keep), hits.end(), [](const Hit& a, const Hit& b) {
            if (a.score != b.score)
                return a.score > b.score;
            if (a.path.size() != b.path.size())
                return a.path.size() < b.path.size();
            return a.path < b.path;
        });
        hits.resize(keep);
    }
    QVariantList out;
    const int limit = recent ? kRecent : kShown;
    for (const Hit& h : hits) {
        const QString path = QString::fromStdString(h.path);
        const QFileInfo info(path);
        if (!info.exists())
            continue;  // the database is a day old at most: gone since
        const QMimeType mime = info.isDir() ? mimes.mimeTypeForName("inode/directory")
                                            : mimes.mimeTypeForFile(info, QMimeDatabase::MatchExtension);
        if (!file_search::filter_accepts(filter_.toStdString(), mime.name().toStdString()))
            continue;
        out.push_back(QVariantMap{
            {"path", path},
            {"name", info.fileName()},
            // A folder's parent says which "src" it is.
            {"parent", QFileInfo(info.path()).fileName()},
            {"folder", info.isDir()},
            {"icon", info.isDir() ? QStringLiteral("folder") : mime.iconName()},
            {"genericIcon", mime.genericIconName()},
            {"mime", mime.name()},
        });
        if (out.size() >= limit)
            break;
    }
    results_ = out;
    recent_ = recent;
    emit resultsChanged();
}

QVariantMap FileSearch::details(const QString& path) const {
    static const QMimeDatabase mimes;
    const QFileInfo info(path);
    if (!info.exists())
        return {};
    const QLocale locale;
    const QMimeType mime = info.isDir() ? mimes.mimeTypeForName("inode/directory") : mimes.mimeTypeForFile(info);
    QString where = info.path();
    if (where.startsWith(QDir::homePath()))
        where.replace(0, QDir::homePath().size(), "~");
    QVariantMap d{
        {"name", info.fileName()},
        {"where", where},
        {"type", mime.comment()},
        {"size", info.isDir() ? QString() : sizeText(info.size())},
        {"modified", locale.toString(info.lastModified(), QLocale::ShortFormat)},
        {"created", info.birthTime().isValid() ? locale.toString(info.birthTime(), QLocale::ShortFormat) : QString()},
        {"icon", info.isDir() ? QStringLiteral("folder") : mime.iconName()},
        {"genericIcon", mime.genericIconName()},
    };
    if (mime.name().startsWith("image/"))
        d["image"] = QUrl::fromLocalFile(path).toString();
    else if (!info.isDir() && mime.inherits("text/plain") && info.size() < 16 * 1024 * 1024) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly))
            d["text"] = QString::fromUtf8(f.read(16 * 1024));
    }
    return d;
}

void FileSearch::open(const QString& path) const {
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void FileSearch::reveal(const QString& path) const {
    QDBusMessage m = QDBusMessage::createMethodCall("org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                                                    "org.freedesktop.FileManager1", "ShowItems");
    m << QStringList{QUrl::fromLocalFile(path).toString()} << QString();
    QDBusConnection::sessionBus().asyncCall(m);
}

void FileSearch::copyFile(const QString& path) const {
    // As file managers copy: pasted into one, it's the file; into a text field, its path.
    auto* data = new QMimeData;
    const QUrl url = QUrl::fromLocalFile(path);
    data->setUrls({url});
    data->setData("x-special/gnome-copied-files", "copy\n" + url.toEncoded());
    data->setText(path);
    QGuiApplication::clipboard()->setMimeData(data);
}

void FileSearch::copyText(const QString& text) const {
    QGuiApplication::clipboard()->setText(text);
}

bool FileSearch::trash(const QString& path) {
    QString where;
    if (!QFile::moveToTrash(path, &where))
        return false;
    results_.removeIf([&](const QVariant& v) { return v.toMap().value("path").toString() == path; });
    emit resultsChanged();
    return true;
}

} // namespace atrium
