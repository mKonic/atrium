#pragma once
// Screen recording with gpu-screen-recorder, into ~/Videos/Recordings.

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace atrium {

class Recorder : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(int seconds READ seconds NOTIFY secondsChanged)
    Q_PROPERTY(QString lastFile READ lastFile NOTIFY recordingChanged)

public:
    explicit Recorder(QObject* parent = nullptr);
    ~Recorder() override;

    bool available() const { return !gsr_.isEmpty(); }
    bool recording() const { return process_ != nullptr; }
    int seconds() const { return recording() ? int(clock_.elapsed() / 1000) : 0; }
    QString lastFile() const { return file_; }

    // The whole of `output` at `fps`, with what the speakers play if `audio`.
    Q_INVOKABLE void start(const QString& output, int fps, bool audio);
    Q_INVOKABLE void stop();

signals:
    void recordingChanged();
    void secondsChanged();
    void failed(const QString& why);
    void saved(const QString& file);

private:
    QString gsr_;
    QProcess* process_ = nullptr;
    QString file_;
    QElapsedTimer clock_;
    QTimer tick_;
};

} // namespace atrium
