#include "file_chooser.hpp"

#include "chooser_core.hpp"
#include "files.hpp"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QMimeDatabase>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>

#include <unistd.h>

#include <cstdio>

namespace atrium {

namespace {

// GTK's mnemonics out of a label: "_Open" is "Open", "__" an underscore.
QString plainLabel(const QString& label) {
    QString out;
    for (qsizetype i = 0; i < label.size(); ++i) {
        if (label[i] == '_' && i + 1 < label.size())
            ++i;
        else if (label[i] == '_')
            continue;
        out += label[i];
    }
    return out;
}

QString displayName(const QString& folder) {
    if (folder == QDir::homePath())
        return "Home";
    const QString name = QFileInfo(folder).fileName();
    return name.isEmpty() ? folder : name;
}

} // namespace

FileChooser::FileChooser(QObject* parent) : QObject(parent) {
    recentFile_ = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/recently-used.xbel";
    // The portal writes the request, then closes; a terminal would never end.
    if (!isatty(STDIN_FILENO)) {
        QFile in;
        if (in.open(stdin, QIODevice::ReadOnly))
            read(in.readAll());
    }
}

// Tests record nothing as recently used unless they say where.
FileChooser::FileChooser(const QByteArray& request, QObject* parent) : QObject(parent), quit_(false) {
    read(request);
}

void FileChooser::read(const QByteArray& request) {
    const QJsonObject q = QJsonDocument::fromJson(request).object();
    mode_ = q.value("mode").toString("open");
    app_ = q.value("app").toString();
    title_ = q.value("title").toString();
    accept_ = plainLabel(q.value("accept").toString());
    multiple_ = mode_ == "open" && q.value("multiple").toBool();
    directory_ = mode_ == "open" && q.value("directory").toBool();
    filters_ = q.value("filters").toArray().toVariantList();
    filter_ = q.value("filter").toInt(filters_.isEmpty() ? -1 : 0);
    if (filter_ < 0 && !filters_.isEmpty())
        filter_ = 0;
    if (filter_ >= filters_.size())
        filter_ = filters_.isEmpty() ? -1 : 0;
    choices_ = q.value("choices").toArray().toVariantList();
    folder_ = q.value("folder").toString();
    name_ = q.value("name").toString();
    file_ = q.value("file").toString();
    files_ = q.value("files").toVariant().toStringList();
    if (!file_.isEmpty() && folder_.isEmpty())
        folder_ = QFileInfo(file_).absolutePath();
    startsSomewhere_ = !folder_.isEmpty() && QFileInfo(folder_).isDir();
    if (!startsSomewhere_)
        folder_ = QDir::homePath();
}

QString FileChooser::action() const {
    if (mode_ == "save")
        return "save";
    if (mode_ == "saveFiles" || directory_)
        return "folder";
    return "open";
}

QString FileChooser::title() const {
    if (!title_.isEmpty())
        return title_;
    if (mode_ == "save")
        return "Save File";
    if (mode_ == "saveFiles")
        return "Select Folder";
    return directory_ ? "Select Folder" : "Open File";
}

QString FileChooser::acceptLabel() const {
    if (!accept_.isEmpty())
        return accept_;
    // xdg-desktop-portal-gtk's defaults.
    if (mode_ == "open")
        return multiple_ ? "Open" : "Select";
    return "Save";
}

QStringList FileChooser::filters() const {
    QStringList out;
    for (const QVariant& f : filters_)
        out << f.toMap().value("name").toString();
    return out;
}

void FileChooser::setFilter(int index) {
    if (index == filter_ || index < -1 || index >= filters_.size())
        return;
    filter_ = index;
    emit filterChanged();
}

QVariantList FileChooser::patterns() const {
    if (filter_ < 0)
        return {};
    return filters_[filter_].toMap().value("patterns").toList();
}

void FileChooser::setChoice(const QString& id, const QString& value) {
    for (QVariant& c : choices_) {
        QVariantMap m = c.toMap();
        if (m.value("id") == id && m.value("value") != value) {
            m["value"] = value;
            c = m;
            emit choicesChanged();
        }
    }
}

void FileChooser::setReadOnly(bool on) {
    if (on == readOnly_)
        return;
    readOnly_ = on;
    emit readOnlyChanged();
}

QVariantMap FileChooser::checkName(const QString& folder, const QString& name, bool isFolder) const {
    const chooser::NameCheck c = chooser::check_name(name.toStdString(), isFolder);
    QString error = QString::fromStdString(c.error);
    if (error.isEmpty() && !name.isEmpty()) {
        const QFileInfo there(QDir(folder).filePath(name));
        if (there.exists() || there.isSymLink())
            error = there.isDir() ? "A folder with that name already exists" : "A file with that name already exists";
    }
    return {{"error", error}, {"warning", QString::fromStdString(c.warning)}};
}

QVariantMap FileChooser::acceptList(const QStringList& picked, const QString& folder) {
    bool allFolders = true, allFiles = true;
    for (const QString& p : picked) {
        const bool dir = QFileInfo(p).isDir();
        allFolders = allFolders && dir;
        allFiles = allFiles && !dir;
    }
    const auto act = action() == "save" ? chooser::Action::Save
                     : action() == "folder" ? chooser::Action::SelectFolder
                                            : chooser::Action::Open;
    switch (chooser::on_accept(act, int(picked.size()), allFolders, allFiles)) {
    case chooser::Respond::Enter:
        return {{"enter", picked.first()}};
    case chooser::Respond::SaveEntry:
        return {{"entry", true}};
    case chooser::Respond::Answer:
        if (mode_ == "saveFiles")
            saveFiles(picked.isEmpty() ? folder : picked.first());
        else
            open(picked.isEmpty() ? QStringList{folder} : picked);
        return {};
    case chooser::Respond::Nothing:
        return {};
    }
    return {};
}

QVariantMap FileChooser::acceptText(const QString& text, const QString& folder, bool replace) {
    if (text.isEmpty())
        return action() == "save" ? QVariantMap{{"focus", true}} : QVariantMap{{"list", true}};
    const std::string home = QDir::homePath().toStdString();
    const chooser::Split split = chooser::split(text.toStdString());
    // Leading and trailing spaces around the name don't count (GTK strips them).
    const QString filePart = QString::fromStdString(split.file).trimmed();
    const QString parent =
        QString::fromStdString(chooser::directory_for_text(text.toStdString(), folder.toStdString(), home));
    if (parent.isEmpty())
        return {{"error", "Invalid file name"}, {"detail", "Only local files can be chosen here."}};
    // "foo/" or "~": the folder itself.
    if (filePart.isEmpty() || chooser::is_directory_shortcut(text.toStdString())) {
        if (action() == "folder") {
            mode_ == "saveFiles" ? saveFiles(parent) : open({parent});
            return {};
        }
        return {{"enter", parent}, {"clear", true}};
    }
    const QString path = QDir::cleanPath(parent + "/" + filePart);
    const QFileInfo info(path);
    const bool exists = info.exists(), isDir = info.isDir();

    if (action() == "open" || (action() == "save" && isDir)) {
        if (isDir)
            return {{"enter", path}, {"clear", true}};
        if (exists) {
            open({path});
            return {};
        }
    }
    if (action() == "folder" && exists) {
        if (!isDir)
            return {{"error", "You may only select folders"},
                    {"detail", "The item that you selected is not a folder try using a different item."}};
        mode_ == "saveFiles" ? saveFiles(path) : open({path});
        return {};
    }
    if (action() == "save" && !exists && filePart.toUtf8().size() > 255)
        return {{"error", "Cannot create file as the filename is too long"}, {"detail", "Try using a shorter name."}};

    // The folder it would be in must be there, and a folder, and open to us.
    const QFileInfo up(parent);
    if (!up.exists())
        return {{"error", "The folder contents could not be displayed"},
                {"detail", QString("%1 doesn't exist.").arg(parent)}};
    if (!up.isDir())
        return {{"error", QString("Cannot create a file under %1 as it is not a folder").arg(parent)},
                {"detail", "You need to choose a valid filename."}};
    if (!up.isExecutable())
        return {{"error", "The folder contents could not be displayed"},
                {"detail", "You do not have access to the specified folder."}};

    if (action() == "open") {
        // Not there yet: apps can still make good use of the name (Emacs).
        open({path});
        return {};
    }
    if (action() == "folder") {
        // Typing a folder that isn't there makes it.
        if (!QDir(parent).mkdir(filePart))
            return {{"error", "The folder could not be created"}, {"detail", "You can't make a folder here."}};
        mode_ == "saveFiles" ? saveFiles(path) : open({path});
        return {};
    }
    // Saving.
    if (exists && !replace)
        return {{"confirm", QString("A file named “%1” already exists.  Do you want to replace it?").arg(filePart)},
                {"detail", QString("The file already exists in “%1”.  Replacing it will overwrite its contents.")
                               .arg(displayName(parent))}};
    save(path);
    return {};
}

QVariantMap FileChooser::makeFolder(const QString& folder, const QString& name) const {
    const QVariantMap check = checkName(folder, name, true);
    if (!check.value("error").toString().isEmpty())
        return {{"error", check.value("error")}};
    if (!QDir(folder).mkdir(name))
        return {{"error", "The folder could not be created"}};
    return {{"path", QDir(folder).filePath(name)}};
}

QVariantMap FileChooser::rename(const QString& path, const QString& name) const {
    const QFileInfo info(path);
    const QString to = QDir(info.absolutePath()).filePath(name);
    if (to == path)
        return {};
    const QVariantMap check = checkName(info.absolutePath(), name, info.isDir());
    if (!check.value("error").toString().isEmpty())
        return {{"error", check.value("error")}};
    if (!QFile::rename(path, to))
        return {{"error", "The file could not be renamed"}};
    return {{"path", to}};
}

QVariantMap FileChooser::trash(const QString& path) const {
    QFile f(path);
    if (!f.moveToTrash())
        return {{"error", "The file could not be moved to the Trash"}};
    return {};
}

QVariantMap FileChooser::remove(const QString& path) const {
    const QFileInfo info(path);
    const bool ok = info.isDir() && !info.isSymLink() ? QDir(path).removeRecursively() : QFile::remove(path);
    if (!ok)
        return {{"error", "The file could not be deleted"}};
    return {};
}

void FileChooser::copyLocations(const QStringList& paths) const {
    // Text (paths, space-separated) and a URI list, as GTK offers both.
    auto* data = new QMimeData;
    QList<QUrl> urls;
    for (const QString& p : paths)
        urls << QUrl::fromLocalFile(p);
    data->setUrls(urls);
    data->setText(paths.join(' '));
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        app->clipboard()->setMimeData(data);
    else
        delete data;
}

void FileChooser::openInFileManager(const QString& path) const {
    QProcess::startDetached("xdg-open", {path});
}

QStringList FileChooser::saveFilesPaths(const QString& folder) const {
    const QDir dir(folder);
    QSet<QString> taken;
    QStringList out;
    for (const QString& f : files_) {
        const std::string name = files::free_file_name(f.toStdString(), [&](const std::string& n) {
            const QString q = QString::fromStdString(n);
            return taken.contains(q) || dir.exists(q);
        });
        taken.insert(QString::fromStdString(name));
        out << dir.filePath(QString::fromStdString(name));
    }
    return out;
}

void FileChooser::open(const QStringList& paths) {
    if (!paths.isEmpty())
        answer(multiple_ ? paths : paths.mid(0, 1));
}

void FileChooser::save(const QString& path) {
    answer({path});
}

void FileChooser::saveFiles(const QString& folder) {
    if (QFileInfo(folder).isDir())
        answer(saveFilesPaths(folder));
}

void FileChooser::addRecent(const QStringList& paths) const {
    // As the GTK portal does: each answer is a recently used file.
    if (recentFile_.isEmpty())
        return;
    QFile in(recentFile_);
    std::string doc;
    if (in.open(QIODevice::ReadOnly))
        doc = in.readAll().toStdString();
    in.close();
    static const QMimeDatabase db;
    const std::string now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();
    for (const QString& p : paths) {
        const std::string uri = chooser::uri_for_path(p.toStdString());
        const QString mime = QFileInfo(p).isDir() ? "inode/directory" : db.mimeTypeForFile(p).name();
        doc = chooser::add_recent(doc, uri, mime.toStdString(), app_.isEmpty() ? "atrium" : app_.toStdString(), now);
    }
    QDir().mkpath(QFileInfo(recentFile_).path());
    QSaveFile out(recentFile_);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QByteArray::fromStdString(doc));
        out.commit();
    }
}

void FileChooser::answer(const QStringList& paths) {
    if (answered_)
        return;
    answered_ = true;
    QJsonArray uris, choices;
    for (const QString& p : paths)
        uris.append(QUrl::fromLocalFile(p).toString(QUrl::FullyEncoded));
    for (const QVariant& c : choices_) {
        const QVariantMap m = c.toMap();
        choices.append(QJsonArray{m.value("id").toString(), m.value("value").toString()});
    }
    answer_ = QJsonDocument(QJsonObject{{"uris", uris}, {"choices", choices}, {"filter", filter_},
                                        {"writable", !readOnly_}})
                  .toJson(QJsonDocument::Compact);
    addRecent(paths);
    if (!quit_)
        return;
    std::fputs((answer_ + '\n').constData(), stdout);
    std::fflush(stdout);
    QCoreApplication::quit();
}

void FileChooser::cancel() {
    // Nothing on stdout: the portal tells the app it was cancelled.
    answered_ = true;
    if (quit_)
        QCoreApplication::quit();
}

} // namespace atrium
