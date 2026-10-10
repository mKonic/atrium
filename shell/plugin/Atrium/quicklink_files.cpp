#include "quicklink_files.hpp"

#include <QCoreApplication>

#include "compositor.hpp"
#include "quicklink_archive_core.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QFile>
#include <QRandomGenerator>
#include <QSaveFile>

namespace atrium {

namespace {

const QString kPortal = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPortalPath = QStringLiteral("/org/freedesktop/portal/desktop");

std::vector<quicklinks::Quicklink> current() {
    std::vector<quicklinks::Quicklink> out;
    for (const QVariant& v : Compositor::instance()->records("quicklinks")) {
        const QVariantMap m = v.toMap();
        out.push_back({m.value("name").toString().toStdString(), m.value("url").toString().toStdString(),
                       m.value("app").toString().toStdString(), m.value("icon").toString().toStdString(),
                       m.value("root", true).toBool()});
    }
    return out;
}

// Messages in the user's language (context "quicklinks"), one and many apart.
QString Q(const char* text) {
    return QCoreApplication::translate("quicklinks", text);
}

} // namespace

QuicklinkFiles* QuicklinkFiles::instance() {
    static auto* self = new QuicklinkFiles;
    return self;
}

QuicklinkFiles::QuicklinkFiles(QObject* parent) : QObject(parent) {}

void QuicklinkFiles::setBusy(bool busy) {
    if (busy == busy_)
        return;
    busy_ = busy;
    emit busyChanged();
}

void QuicklinkFiles::importFile(const QUrl& file) {
    QFile f(file.isLocalFile() ? file.toLocalFile() : file.toString());
    if (!f.open(QIODevice::ReadOnly)) {
        emit finished(Q("Couldn't read %1.").arg(f.fileName()), true);
        return;
    }
    const QByteArray data = f.read(16 * 1024 * 1024);
    const auto decoded = quicklinks::decode(std::string_view(data.constData(), size_t(data.size())));
    if (const auto* error = std::get_if<std::string>(&decoded)) {
        emit finished(QString::fromStdString(*error), true);
        return;
    }
    const quicklinks::Merge m = quicklinks::merge(std::get<std::vector<quicklinks::Quicklink>>(decoded), current());
    for (const quicklinks::Quicklink& q : m.additions)
        Compositor::instance()->addRecord("quicklinks", {{"name", QString::fromStdString(q.name)},
                                                         {"url", QString::fromStdString(q.link)},
                                                         {"app", QString::fromStdString(q.app)},
                                                         {"icon", QString::fromStdString(q.icon)},
                                                         {"root", q.root}});
    const int n = int(m.additions.size());
    QString note = n == 0 ? Q("Nothing new to import.") : n == 1 ? Q("1 quicklink imported.") : Q("%1 quicklinks imported.").arg(n);
    if (m.skipped)
        note += " " + (m.skipped == 1 ? Q("1 was already here or incomplete.") : Q("%1 were already here or incomplete.").arg(m.skipped));
    emit finished(note, n == 0);
}

void QuicklinkFiles::exportAll() {
    if (busy_)
        return;
    // The answer comes as a signal on a request path made of our bus name
    // and a token: listened for before asking, so it can't be missed.
    const QString token = "atrium" + QString::number(QRandomGenerator::global()->generate());
    QString sender = QDBusConnection::sessionBus().baseService().mid(1);
    sender.replace('.', '_');
    request_ = kPortalPath + "/request/" + sender + "/" + token;
    QDBusConnection::sessionBus().connect(kPortal, request_, "org.freedesktop.portal.Request", "Response", this,
                                          SLOT(saveChosen(uint, QVariantMap)));
    QDBusMessage ask = QDBusMessage::createMethodCall(kPortal, kPortalPath, "org.freedesktop.portal.FileChooser",
                                                      "SaveFile");
    ask << QString() << Q("Export Quicklinks")
        << QVariantMap{{"handle_token", token}, {"current_name", QStringLiteral("quicklinks.json")}, {"modal", true}};
    const QDBusReply<QDBusObjectPath> reply = QDBusConnection::sessionBus().call(ask);
    if (!reply.isValid()) {
        QDBusConnection::sessionBus().disconnect(kPortal, request_, "org.freedesktop.portal.Request", "Response", this,
                                                 SLOT(saveChosen(uint, QVariantMap)));
        emit finished(Q("There's no save dialog to ask where (xdg-desktop-portal isn't running)."), true);
        return;
    }
    setBusy(true);
}

void QuicklinkFiles::saveChosen(uint response, const QVariantMap& results) {
    QDBusConnection::sessionBus().disconnect(kPortal, request_, "org.freedesktop.portal.Request", "Response", this,
                                             SLOT(saveChosen(uint, QVariantMap)));
    setBusy(false);
    const QStringList uris = results.value("uris").toStringList();
    if (response != 0 || uris.isEmpty())
        return;  // cancelled
    const QString path = QUrl(uris.first()).toLocalFile();
    const auto list = current();
    QSaveFile out(path);
    const std::string text = quicklinks::encode(list);
    if (!out.open(QIODevice::WriteOnly) || out.write(text.data(), qint64(text.size())) != qint64(text.size()) ||
        !out.commit()) {
        emit finished(Q("Couldn't write %1.").arg(path), true);
        return;
    }
    emit finished(list.size() == 1 ? Q("1 quicklink exported to %1.").arg(path)
                                   : Q("%1 quicklinks exported to %2.").arg(list.size()).arg(path),
                  false);
}

} // namespace atrium
