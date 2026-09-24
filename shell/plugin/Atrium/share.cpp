#include "share.hpp"

#include "compositor.hpp"
#include "share_core.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#include <unistd.h>

#include <cstdio>

namespace atrium {

ShareChooser::ShareChooser(QObject* parent) : QObject(parent) {
    // The portal writes every line, then closes; a terminal would never end.
    if (!isatty(STDIN_FILENO)) {
        QFile in;
        if (in.open(stdin, QIODevice::ReadOnly)) {
            const QByteArray text = in.readAll();
            for (const share::Source& s : share::parse(text.toStdString())) {
                sources_.append(QVariantMap{
                    {"kind", s.kind == share::Source::Kind::Screen ? "screen" : "window"},
                    {"line", QString::fromStdString(s.line)},
                    {"name", QString::fromStdString(s.name)},
                    {"text", QString::fromStdString(s.text)},
                    {"appId", QString()},
                    {"thumbnail", QUrl()},
                });
            }
        }
    }
    matchApps();
    connect(Compositor::instance(), &Compositor::windowsChanged, this, &ShareChooser::matchApps);
    if (dir_.isValid() && !QStandardPaths::findExecutable("grim").isEmpty())
        for (int i = 0; i < sources_.size(); ++i)
            takeThumbnail(i);
}

QVariantList ShareChooser::list(const QString& kind) const {
    QVariantList out;
    for (const QVariant& v : sources_)
        if (v.toMap().value("kind") == kind)
            out.append(v);
    return out;
}

void ShareChooser::matchApps() {
    QHash<QString, QString> apps;
    for (const QVariant& w : Compositor::instance()->windows()) {
        const QVariantMap m = w.toMap();
        apps.insert(m.value("identifier").toString(), m.value("app_id").toString());
    }
    bool any = false;
    for (QVariant& v : sources_) {
        QVariantMap m = v.toMap();
        const QString app = apps.value(m.value("name").toString());
        if (m.value("kind") == "window" && !app.isEmpty() && m.value("appId") != app) {
            m["appId"] = app;
            v = m;
            any = true;
        }
    }
    if (any)
        emit changed();
}

void ShareChooser::takeThumbnail(int i) {
    const QVariantMap m = sources_[i].toMap();
    const QString file = dir_.filePath(QString::number(i) + ".png");
    const bool screen = m.value("kind") == "screen";
    auto* grim = new QProcess(this);
    connect(grim, &QProcess::finished, this, [this, grim, i, file](int code) {
        grim->deleteLater();
        if (code != 0 || !QFile::exists(file))
            return;
        QVariantMap m = sources_[i].toMap();
        m["thumbnail"] = QUrl::fromLocalFile(file);
        sources_[i] = m;
        emit changed();
    });
    grim->start("grim", {"-s", screen ? "0.2" : "0.35", "-l", "1", screen ? "-o" : "-T", m.value("name").toString(), file});
}

void ShareChooser::choose(const QString& line) {
    if (answered_)
        return;
    answered_ = true;
    std::fputs((line + u'\n').toUtf8().constData(), stdout);
    std::fflush(stdout);
    QCoreApplication::quit();
}

void ShareChooser::cancel() {
    // Nothing on stdout: the portal tells the app the user said no.
    answered_ = true;
    QCoreApplication::quit();
}

} // namespace atrium
