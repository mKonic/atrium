#include "portal_print.hpp"

#include "printers_core.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>

#include <unistd.h>

namespace atrium {

namespace {

// A{sv} from the app, as the dialog's JSON: GTK's print settings are all
// strings, a page setup's sizes numbers.
QJsonObject json(const QVariantMap& m) {
    return QJsonObject::fromVariantMap(m);
}

QVariantMap settingsOf(const QJsonObject& o) {
    QVariantMap out;
    for (auto it = o.begin(); it != o.end(); ++it)
        out.insert(it.key(), it.value().toString());
    return out;
}

QVariantMap pageSetupOf(const QJsonObject& o) {
    QVariantMap out;
    for (auto it = o.begin(); it != o.end(); ++it)
        out.insert(it.key(), it->isDouble() ? QVariant(it->toDouble()) : QVariant(it->toString()));
    return out;
}

QByteArray readAll(const QDBusUnixFileDescriptor& fd) {
    QFile f;
    if (!fd.isValid() || !f.open(::dup(fd.fileDescriptor()), QIODevice::ReadOnly, QFileDevice::AutoCloseHandle))
        return {};
    return f.readAll();
}

} // namespace

bool sendPrintJob(const QJsonObject& job, const QString& title, const QByteArray& document) {
    if (const QString pdf = job.value("pdf").toString(); !pdf.isEmpty()) {
        QSaveFile f(pdf);
        return f.open(QIODevice::WriteOnly) && f.write(document) == document.size() && f.commit();
    }
    // The app rendered the pages, their order and orientation already; what
    // only the printer does is left: copies, the paper, sides and colour.
    printers::PrintJob j;
    j.printer = job.value("printer").toString().toStdString();
    j.title = title.toStdString();
    j.copies = qMax(1, job.value("copies").toInt(1));
    j.paper = job.value("paper").toString().toStdString();
    j.duplex = job.value("duplex").toString().toStdString();
    j.color_model = job.value("color").toString().toStdString();
    QStringList args;
    for (const std::string& a : printers::lp_args(j))
        args << QString::fromStdString(a);
    QProcess lp;
    lp.start("lp", args);
    if (!lp.waitForStarted(5000))
        return false;
    lp.write(document);
    lp.closeWriteChannel();
    if (!lp.waitForFinished(60000)) {
        lp.kill();
        return false;
    }
    if (lp.exitStatus() != QProcess::NormalExit || lp.exitCode() != 0) {
        qWarning("atrium-portal: lp failed: %s", lp.readAllStandardError().trimmed().constData());
        return false;
    }
    return true;
}

void PrintAdaptor::ask(const QDBusObjectPath& handle, const QString& title, const QVariantMap& settings,
                       const QVariantMap& pageSetup, const QVariantMap& options,
                       std::function<QVariantList(const QJsonObject&, QVariantMap)> done) {
    const QJsonObject question{
        {"title", title},
        {"accept", options.value("accept_label").toString()},
        {"settings", json(settings)},
        {"pageSetup", json(pageSetup)},
    };
    // 0 printed (or ready to), 1 cancelled, 2 failed.
    askShell(handle.path(), "print.qml", QJsonDocument(question).toJson(QJsonDocument::Compact), {},
             [done](const std::optional<QByteArray>& out) -> QVariantList {
                 if (!out)
                     return {uint(2), QVariantMap()};
                 const QJsonObject reply = QJsonDocument::fromJson(*out).object();
                 if (!reply.contains("job"))
                     return {uint(1), QVariantMap()};
                 QVariantMap results{{"settings", settingsOf(reply.value("settings").toObject())},
                                     {"page-setup", pageSetupOf(reply.value("pageSetup").toObject())}};
                 return done(reply.value("job").toObject(), results);
             });
}

uint PrintAdaptor::PreparePrint(const QDBusObjectPath& handle, const QString&, const QString&,
                                const QString& title, const QVariantMap& settings, const QVariantMap& pageSetup,
                                const QVariantMap& options, QVariantMap&) {
    ask(handle, title, settings, pageSetup, options, [this](const QJsonObject& job, QVariantMap results) {
        const uint token = next_++;
        jobs_.insert(token, job);
        results.insert("token", token);
        return QVariantList{uint(0), results};
    });
    return 2;  // unused: the reply goes later
}

uint PrintAdaptor::Print(const QDBusObjectPath& handle, const QString&, const QString&, const QString& title,
                         const QDBusUnixFileDescriptor& fd, const QVariantMap& options, QVariantMap&) {
    const QByteArray document = readAll(fd);
    const uint token = options.value("token").toUInt();
    if (jobs_.contains(token))
        return sendPrintJob(jobs_.take(token), title, document) ? 0 : 2;
    // Not prepared: asked now.
    ask(handle, title, {}, {}, options, [title, document](const QJsonObject& job, const QVariantMap&) {
        return QVariantList{uint(sendPrintJob(job, title, document) ? 0 : 2), QVariantMap()};
    });
    return 2;  // unused: the reply goes later
}

} // namespace atrium
