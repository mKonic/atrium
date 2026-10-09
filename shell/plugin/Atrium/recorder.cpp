#include "recorder.hpp"

#include "capture_core.hpp"
#include "compositor.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>

#include <csignal>

namespace atrium {

Recorder::Recorder(QObject* parent) : QObject(parent) {
    program_ = QString::fromStdString(capture::atrium_program(QCoreApplication::applicationDirPath().toStdString(), "atrium-record"));
    tick_.setInterval(1000);
    connect(&tick_, &QTimer::timeout, this, &Recorder::secondsChanged);
}

Recorder::~Recorder() {
    stop();
}

void Recorder::start(const QString& output, int fps, bool audio) {
    if (recording() || !available())
        return;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + "/Recordings";
    QDir().mkpath(dir);
    file_ = dir + "/recording_" + QDateTime::currentDateTime().toString("yyyyMMdd_HH-mm-ss") + ".mp4";

    QStringList args{"-w", output, "-f", QString::number(std::max(fps, 30)), "-o", file_};
    if (audio)
        args << "-a" << "default_output";

    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    connect(process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        const bool early = clock_.elapsed() < 1500;
        process_->deleteLater();
        process_ = nullptr;
        tick_.stop();
        emit recordingChanged();
        if (early && code != 0)
            emit failed(QString("atrium-record stopped at once (exit %1)").arg(code));
        else
            emit saved(file_);
    });
    process_->start(program_, args);
    clock_.start();
    tick_.start();
    emit recordingChanged();
}

void Recorder::toggle() {
    if (recording()) {
        stop();
        return;
    }
    if (!available())
        return;
    Compositor* c = Compositor::instance();
    const QVariantMap out = c->focusedOutput().toMap();
    start(out.value("name").toString(), qRound(out.value("refresh", 60).toDouble()),
          c->settings().value("recording.audio").toBool());
}

void Recorder::stop() {
    // SIGINT lets it finish the file properly.
    if (process_)
        ::kill(pid_t(process_->processId()), SIGINT);
}

} // namespace atrium
