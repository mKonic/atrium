#include "app_chooser.hpp"

#include "desktop_entry_core.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QUrl>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

namespace atrium {

namespace {

// What a link is, by its scheme (x-scheme-handler/https).
QString linkKind(const QString& scheme) {
    if (scheme == "http" || scheme == "https")
        return "Web page";
    if (scheme == "mailto")
        return "Email address";
    if (scheme == "tel" || scheme == "callto")
        return "Phone number";
    return QString("“%1” link").arg(scheme);
}

QString messagesLocale() {
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"})
        if (const char* v = std::getenv(var); v && *v)
            return QString::fromLocal8Bit(v);
    return {};
}

} // namespace

AppChooser::AppChooser(QObject* parent) : QObject(parent) {
    if (isatty(STDIN_FILENO))
        return;
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    input_ = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
    connect(input_, &QSocketNotifier::activated, this, &AppChooser::readInput);
    readInput();
}

AppChooser::AppChooser(bool, QObject* parent) : QObject(parent), quit_(false) {}

void AppChooser::readInput() {
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
        if (n > 0) {
            pending_.append(buf, n);
            continue;
        }
        if (n == 0)  // nothing more will come (the portal ends the dialog itself)
            input_->setEnabled(false);
        break;
    }
    for (qsizetype nl; (nl = pending_.indexOf('\n')) >= 0;) {
        feed(pending_.left(nl));
        pending_.remove(0, nl + 1);
    }
}

void AppChooser::feed(const QByteArray& line) {
    const QJsonObject q = QJsonDocument::fromJson(line).object();
    if (q.isEmpty())
        return;
    if (q.contains("last"))
        last_ = q.value("last").toString();
    if (q.contains("filename") || q.contains("uri")) {
        const QString file = q.value("filename").toString();
        const QUrl uri(q.value("uri").toString());
        subject_ = !file.isEmpty() ? file.section('/', -1)
                   : uri.isLocalFile() ? uri.fileName()
                                       : uri.toString(QUrl::RemoveUserInfo);
    }
    if (const QString type = q.value("contentType").toString(); !type.isEmpty()) {
        const QMimeType mime = QMimeDatabase().mimeTypeForName(type);
        kind_ = type.startsWith("x-scheme-handler/") ? linkKind(type.section('/', 1))
                : mime.isValid()                     ? mime.comment()
                                                     : type;
    }
    // The last one used first, then the rest as the portal ranks them.
    QStringList ids = q.value("choices").toVariant().toStringList();
    if (ids.removeAll(last_) > 0)
        ids.prepend(last_);
    choices_ = ids;
    // Names and icons from the desktop files; an id beside a name two share.
    const std::string locale = messagesLocale().toStdString();
    apps_.clear();
    QHash<QString, int> named;
    for (const QString& id : std::as_const(choices_)) {
        QVariantMap app{{"id", id}, {"name", id}, {"icon", id}, {"detail", QString()}};
        QFile f(QStandardPaths::locate(QStandardPaths::ApplicationsLocation, id + ".desktop"));
        if (f.open(QIODevice::ReadOnly))
            if (const auto e = desktop_entry::parse(f.readAll().toStdString(), locale)) {
                if (!e->name.empty())
                    app["name"] = QString::fromStdString(e->name);
                if (!e->icon.empty())
                    app["icon"] = QString::fromStdString(e->icon);
            }
        ++named[app["name"].toString()];
        apps_.append(app);
    }
    for (QVariant& v : apps_) {
        QVariantMap app = v.toMap();
        if (named.value(app["name"].toString()) > 1) {
            app["detail"] = app["id"];
            v = app;
        }
    }
    emit choicesChanged();
}

void AppChooser::choose(const QString& id) {
    if (answered_ || !choices_.contains(id))
        return;
    answer_ = QJsonDocument(QJsonObject{{"choice", id}}).toJson(QJsonDocument::Compact);
    if (quit_) {
        std::fputs((answer_ + '\n').constData(), stdout);
        std::fflush(stdout);
    }
    finish();
}

void AppChooser::cancel() {
    // Nothing on stdout: the portal tells the app it was cancelled.
    finish();
}

void AppChooser::finish() {
    answered_ = true;
    if (quit_)
        QCoreApplication::quit();
}

} // namespace atrium
