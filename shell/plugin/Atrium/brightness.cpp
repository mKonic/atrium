#include "brightness.hpp"

#include <QDBusInterface>
#include <QDBusConnection>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>

namespace atrium {

namespace {

int readInt(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed().toInt() : 0;
}

} // namespace

Brightness::Brightness(QObject* parent) : QObject(parent) {
    // A laptop panel.
    const QStringList panels = QDir("/sys/class/backlight").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (!panels.isEmpty()) {
        backlight_ = "/sys/class/backlight/" + panels.first();
        backlightMax_ = readInt(backlight_ + "/max_brightness");
        if (backlightMax_ > 0)
            value_ = readInt(backlight_ + "/brightness") * 100 / backlightMax_;
    }
    ddcutil_ = QStandardPaths::findExecutable("ddcutil");
    detect();
}

// Which monitors answer DDC/CI. Takes a second or two; nothing waits on it.
void Brightness::detect() {
    if (ddcutil_.isEmpty())
        return;
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p] {
        static const QRegularExpression display(R"(^Display (\d+))", QRegularExpression::MultilineOption);
        auto it = display.globalMatch(QString::fromUtf8(p->readAllStandardOutput()));
        while (it.hasNext())
            ddc_.push_back(it.next().captured(1));
        emit changed();
        readCurrent();
        p->deleteLater();
    });
    p->start(ddcutil_, {"detect", "--brief"});
}

void Brightness::readCurrent() {
    if (ddc_.empty())
        return;
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p] {
        // "VCP 10 C 80 100": current, max.
        const QStringList parts = QString::fromUtf8(p->readAllStandardOutput()).split(' ', Qt::SkipEmptyParts);
        if (parts.size() >= 5 && parts[4].toInt() > 0) {
            value_ = parts[3].toInt() * 100 / parts[4].toInt();
            emit changed();
        }
        p->deleteLater();
    });
    p->start(ddcutil_, {"getvcp", "10", "--brief", "--display", ddc_.front()});
}

void Brightness::set(int percent) {
    percent = std::clamp(percent, 0, 100);
    value_ = percent;
    emit changed();
    if (!backlight_.isEmpty() && backlightMax_ > 0) {
        // logind lets the session set its own backlight.
        QDBusInterface session("org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
                               "org.freedesktop.login1.Session", QDBusConnection::systemBus());
        session.asyncCall("SetBrightness", QString("backlight"), backlight_.section('/', -1),
                          uint(percent * backlightMax_ / 100));
    }
    if (ddc_.empty())
        return;
    pending_ = percent;
    writeNext();
}

void Brightness::writeNext() {
    if (busy_ || !pending_)
        return;
    const int percent = *pending_;
    pending_.reset();
    busy_ = true;
    emit changed();
    // All monitors, one after another, in one shell so it is one write.
    QStringList script;
    for (const QString& d : ddc_)
        script << QString("'%1' setvcp 10 %2 --display %3 --noverify").arg(ddcutil_).arg(percent).arg(d);
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p] {
        busy_ = false;
        emit changed();
        writeNext();  // the slider moved on meanwhile
        p->deleteLater();
    });
    p->start("/bin/sh", {"-c", script.join(" ; ")});
}

} // namespace atrium
