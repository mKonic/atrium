#include "window_shots.hpp"

#include "capture_core.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QUrl>

namespace atrium {

WindowShots::WindowShots(QObject* parent) : QObject(parent) {}

void WindowShots::take(const QString& identifier) {
    if (identifier.isEmpty() || taking_.contains(identifier) || !dir_.isValid())
        return;
    taking_.insert(identifier);
    // A new name each time, so an Image showing the old one reloads.
    const QString file = dir_.filePath(QString("%1.png").arg(++serial_));
    auto* shot = new QProcess(this);
    connect(shot, &QProcess::finished, this, [this, shot, identifier, file](int code) {
        shot->deleteLater();
        taking_.remove(identifier);
        if (code != 0 || !QFile::exists(file))
            return;
        const QString old = shots_.value(identifier).toUrl().toLocalFile();
        shots_[identifier] = QUrl::fromLocalFile(file);
        if (!old.isEmpty())
            QFile::remove(old);
        emit shotsChanged();
    });
    connect(shot, &QProcess::errorOccurred, this, [this, identifier](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            taking_.remove(identifier);
    });
    shot->start(QString::fromStdString(capture::screenshot_program(QCoreApplication::applicationDirPath().toStdString())),
                {"-s", "0.35", "-l", "1", "-T", identifier, file});
}

} // namespace atrium
