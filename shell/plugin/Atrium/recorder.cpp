#include "recorder.hpp"

#include "compositor.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>

#include <csignal>

namespace atrium {

Recorder::Recorder(QObject* parent) : QObject(parent) {
    // Beside a shell run from the build tree, its own; else the installed one.
    const QString built = QCoreApplication::applicationDirPath() + "/../../record/atrium-record";
    program_ = QFileInfo(built).isExecutable() ? built : QStandardPaths::findExecutable("atrium-record");
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

    QStringList args{"-o", output, "-f", QString::number(std::max(fps, 30))};
    if (audio)
        args << "-a";
    args << file_;

    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    connect(process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        const bool early = clock_.elapsed() < 1500;
        process_->deleteLater();
        process_ = nullptr;
        tick_.stop();
        emit recordingChanged();
        if (early && code != 0)
            emit failed(QString("Recording stopped at once (atrium-record exit %1)").arg(code));
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
