#pragma once
// While `enabled`, keys typed into `window` reach it instead of atrium's
// own shortcuts (keyboard-shortcuts-inhibit): the Shortcuts page records
// Super+Space as a key, not as the launcher opening.

#include <QObject>
#include <QPointer>
#include <QWindow>

struct zwp_keyboard_shortcuts_inhibitor_v1;

namespace atrium::shell {

class ShortcutInhibitor : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)  // the compositor agreed

public:
    using QObject::QObject;
    ~ShortcutInhibitor() override;

    QObject* window() const { return window_; }
    void setWindow(QObject* window);
    bool enabled() const { return enabled_; }
    void setEnabled(bool on);
    bool active() const { return active_; }

    void setActive(bool on);

signals:
    void windowChanged();
    void enabledChanged();
    void activeChanged();

private:
    void update();
    void drop();

    QPointer<QWindow> window_;
    bool enabled_ = false;
    bool active_ = false;
    zwp_keyboard_shortcuts_inhibitor_v1* inhibitor_ = nullptr;
};

} // namespace atrium::shell
