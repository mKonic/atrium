#pragma once
// Reactive slices of the compositor state for QML. A binding that calls a
// C++ function cannot know when to re-run; these objects recompute when the
// compositor state changes and notify like any property.

#include <QObject>
#include <QVariant>

namespace atrium {

class Compositor;

// What one output shows: `OutputState { name: screen.name }`.
class OutputState : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
    Q_PROPERTY(int activeSpace READ activeSpace NOTIFY changed)
    Q_PROPERTY(bool fullscreen READ fullscreen NOTIFY changed)
    Q_PROPERTY(QVariantList spaces READ spaces NOTIFY changed)

public:
    explicit OutputState(QObject* parent = nullptr);

    QString name() const { return name_; }
    void setName(const QString& name);
    int activeSpace() const { return active_; }
    bool fullscreen() const { return fullscreen_; }
    QVariantList spaces() const { return spaces_; }

signals:
    void nameChanged();
    void changed();

private:
    void update();

    QString name_;
    int active_ = 1;
    bool fullscreen_ = false;
    QVariantList spaces_;
};

// The windows on one space of one output, most recently used first:
// `SpaceWindows { output: "DP-1"; number: 3 }`.
class SpaceWindows : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString output READ output WRITE setOutput NOTIFY outputChanged)
    Q_PROPERTY(int number READ number WRITE setNumber NOTIFY numberChanged)
    Q_PROPERTY(QVariantList windows READ windows NOTIFY windowsChanged)

public:
    explicit SpaceWindows(QObject* parent = nullptr);

    QString output() const { return output_; }
    void setOutput(const QString& output);
    int number() const { return number_; }
    void setNumber(int number);
    QVariantList windows() const { return windows_; }

signals:
    void outputChanged();
    void numberChanged();
    void windowsChanged();

private:
    void update();

    QString output_;
    int number_ = 0;
    QVariantList windows_;
};

} // namespace atrium
