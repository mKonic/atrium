#pragma once
// Small pictures of open windows, for pickers that choose one (Settings'
// rules and apps), taken as the share picker takes them: atrium-screenshot
// -T with the window's foreign-toplevel identifier, whole even when covered
// or on another space.
//
//   WindowShots { id: shots }  shots.take(w.identifier)  shots.shots[w.identifier]

#include <QObject>
#include <QSet>
#include <QTemporaryDir>
#include <QVariantMap>

namespace atrium {

class WindowShots : public QObject {
    Q_OBJECT
    // identifier -> file URL of its picture, once taken.
    Q_PROPERTY(QVariantMap shots READ shots NOTIFY shotsChanged)

public:
    explicit WindowShots(QObject* parent = nullptr);

    QVariantMap shots() const { return shots_; }
    // Takes (or retakes) one window's picture; shotsChanged when it's there.
    Q_INVOKABLE void take(const QString& identifier);

signals:
    void shotsChanged();

private:
    QTemporaryDir dir_;
    QVariantMap shots_;
    QSet<QString> taking_;
    int serial_ = 0;
};

} // namespace atrium
