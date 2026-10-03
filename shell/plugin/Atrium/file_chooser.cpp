#include "file_chooser.hpp"

#include "files.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
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

} // namespace

FileChooser::FileChooser(QObject* parent) : QObject(parent) {
    // The portal writes the request, then closes; a terminal would never end.
    if (!isatty(STDIN_FILENO)) {
        QFile in;
        if (in.open(stdin, QIODevice::ReadOnly))
            read(in.readAll());
    }
    if (folder_.isEmpty() || !QFileInfo(folder_).isDir())
        folder_ = QDir::homePath();
}

FileChooser::FileChooser(const QByteArray& request, QObject* parent) : QObject(parent), quit_(false) {
    read(request);
    if (folder_.isEmpty() || !QFileInfo(folder_).isDir())
        folder_ = QDir::homePath();
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
    files_ = q.value("files").toVariant().toStringList();
}

QString FileChooser::title() const {
    if (!title_.isEmpty())
        return title_;
    if (mode_ == "save")
        return "Save";
    if (mode_ == "saveFiles")
        return "Choose Where to Save";
    return directory_ ? "Choose a Folder" : "Open";
}

QString FileChooser::acceptLabel() const {
    if (!accept_.isEmpty())
        return accept_;
    if (mode_ == "open")
        return directory_ ? "Choose" : "Open";
    return "Save";
}

QStringList FileChooser::filters() const {
    QStringList out;
    for (const QVariant& f : filters_)
        out << f.toMap().value("name").toString();
    return out;
}

void FileChooser::setFilter(int index) {
    if (index == filter_ || index < 0 || index >= filters_.size())
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

QString FileChooser::saveProblem(const QString& folder, const QString& name) const {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return "Type a name to save it under.";
    if (!files::valid_name(trimmed.toStdString()))
        return "A name can't have “/” in it, or be “.” or “..”.";
    const QFileInfo dir(folder);
    if (!dir.isDir() || !dir.isWritable())
        return "You can't save in this folder.";
    if (QFileInfo(QDir(folder).filePath(trimmed)).isDir())
        return "There's a folder with that name here.";
    return {};
}

bool FileChooser::exists(const QString& folder, const QString& name) const {
    return QFileInfo::exists(QDir(folder).filePath(name.trimmed()));
}

QStringList FileChooser::saveFilesPaths(const QString& folder) const {
    // Each under its own name, numbered past what's there (and past each
    // other: two files can't be saved as one).
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

QVariantMap FileChooser::accept(const QStringList& picked, const QString& folder, const QString& name,
                                bool replace) {
    QStringList dirs, plain;
    for (const QString& p : picked)
        (QFileInfo(p).isDir() ? dirs : plain) << p;
    if (mode_ == "save") {
        // A folder picked is gone into, not saved over.
        if (dirs.size() == 1 && plain.isEmpty())
            return {{"enter", dirs.first()}};
        if (const QString problem = saveProblem(folder, name); !problem.isEmpty())
            return {{"error", problem}};
        if (exists(folder, name) && !replace)
            return {{"confirm", QString("“%1” already exists. Do you want to replace it?").arg(name.trimmed())}};
        save(folder, name);
        return {};
    }
    if (mode_ == "saveFiles") {
        const QString into = dirs.size() == 1 ? dirs.first() : folder;
        if (!QFileInfo(into).isWritable())
            return {{"error", "You can't save in this folder."}};
        saveFiles(into);
        return {};
    }
    if (directory_) {
        // The folders picked, or the one it's in.
        open(dirs.isEmpty() ? QStringList{folder} : dirs);
        return {};
    }
    if (plain.isEmpty())
        return dirs.size() == 1 ? QVariantMap{{"enter", dirs.first()}} : QVariantMap{};
    open(plain);
    return {};
}

QVariantMap FileChooser::makeFolder(const QString& folder, const QString& name) const {
    const QString trimmed = name.trimmed();
    if (!files::valid_name(trimmed.toStdString()))
        return {{"error", "A name can't be empty, have “/” in it, or be “.” or “..”."}};
    if (QFileInfo::exists(QDir(folder).filePath(trimmed)))
        return {{"error", "There's already something with that name here."}};
    if (!QDir(folder).mkdir(trimmed))
        return {{"error", "You can't make a folder here."}};
    return {{"path", QDir(folder).filePath(trimmed)}};
}

void FileChooser::open(const QStringList& paths) {
    if (!paths.isEmpty())
        answer(multiple_ ? paths : paths.mid(0, 1));
}

void FileChooser::save(const QString& folder, const QString& name) {
    if (saveProblem(folder, name).isEmpty())
        answer({QDir(folder).filePath(name.trimmed())});
}

void FileChooser::saveFiles(const QString& folder) {
    if (QFileInfo(folder).isDir())
        answer(saveFilesPaths(folder));
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
    answer_ = QJsonDocument(QJsonObject{{"uris", uris}, {"choices", choices}, {"filter", filter_}})
                  .toJson(QJsonDocument::Compact);
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
