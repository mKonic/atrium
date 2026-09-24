#pragma once
// Screens as QML sees them: `Shell.screens`, one ShellScreen per output,
// the same object for as long as the output exists.

#include <QList>
#include <QObject>
#include <QPointer>
#include <QScreen>

namespace atrium::shell {

class ShellScreen : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(QString model READ model CONSTANT)
    Q_PROPERTY(int x READ x NOTIFY geometryChanged)
    Q_PROPERTY(int y READ y NOTIFY geometryChanged)
    Q_PROPERTY(int width READ width NOTIFY geometryChanged)
    Q_PROPERTY(int height READ height NOTIFY geometryChanged)
    Q_PROPERTY(qreal devicePixelRatio READ devicePixelRatio NOTIFY geometryChanged)

public:
    explicit ShellScreen(QScreen* screen, QObject* parent = nullptr);

    QScreen* screen() const { return screen_; }
    QString name() const { return screen_ ? screen_->name() : QString(); }
    QString model() const { return screen_ ? screen_->model() : QString(); }
    int x() const { return screen_ ? screen_->geometry().x() : 0; }
    int y() const { return screen_ ? screen_->geometry().y() : 0; }
    int width() const { return screen_ ? screen_->geometry().width() : 0; }
    int height() const { return screen_ ? screen_->geometry().height() : 0; }
    qreal devicePixelRatio() const { return screen_ ? screen_->devicePixelRatio() : 1.0; }

signals:
    void geometryChanged();

private:
    QPointer<QScreen> screen_;
};

// Keeps one ShellScreen per QScreen, in the application's order.
class Screens : public QObject {
    Q_OBJECT

public:
    static Screens* instance();

    QList<ShellScreen*> list() const { return list_; }
    ShellScreen* forScreen(QScreen* screen) const;

signals:
    void changed();

private:
    Screens();
    void add(QScreen* screen);
    void remove(QScreen* screen);

    QList<ShellScreen*> list_;
};

} // namespace atrium::shell
