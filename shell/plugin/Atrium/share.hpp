#pragma once
// The screen-share chooser (share.qml), run by xdg-desktop-portal-wlr when an
// app asks to share the screen: it reads the screens and windows on offer
// from stdin, and `choose(line)` answers on stdout. Each source gets a small
// picture of itself, taken with grim.
//
//   sources: [{ kind: "screen"|"window", line, name, text, appId, thumbnail }]

#include <QObject>
#include <QTemporaryDir>
#include <QVariantList>

namespace atrium {

class ShareChooser : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList screens READ screens NOTIFY changed)
    Q_PROPERTY(QVariantList windows READ windows NOTIFY changed)

public:
    explicit ShareChooser(QObject* parent = nullptr);

    QVariantList screens() const { return list("screen"); }
    QVariantList windows() const { return list("window"); }

    Q_INVOKABLE void choose(const QString& line);
    Q_INVOKABLE void cancel();

signals:
    void changed();

private:
    QVariantList list(const QString& kind) const;
    void matchApps();          // app ids for windows, from the compositor
    void takeThumbnail(int i);

    QVariantList sources_;
    QTemporaryDir dir_;
    bool answered_ = false;
};

} // namespace atrium
