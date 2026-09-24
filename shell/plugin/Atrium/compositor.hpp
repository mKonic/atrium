#pragma once
// The shell's connection to atrium: its control socket (JSON lines), kept as
// live state for QML. One connection listens for events and applies each one
// as it comes (a window event touches that window only); another sends
// requests and matches replies by id.

#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>
#include <QVariant>

#include <functional>
#include <map>

namespace atrium {

class Compositor : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList windows READ windows NOTIFY windowsChanged)
    Q_PROPERTY(QVariantList spaces READ spaces NOTIFY spacesChanged)
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY outputsChanged)
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY settingsChanged)
    // Every setting as the Settings app draws it: key, type, title,
    // description, page, default, min/max, choices.
    Q_PROPERTY(QVariantList schema READ schema NOTIFY schemaChanged)
    // The registry's records: apps (where they open, Dock pins), pattern
    // rules, shortcuts.
    Q_PROPERTY(QVariantList apps READ apps NOTIFY appsChanged)
    Q_PROPERTY(QVariantList rules READ rules NOTIFY rulesChanged)
    Q_PROPERTY(QVariantList shortcuts READ shortcuts NOTIFY shortcutsChanged)
    Q_PROPERTY(QStringList actions READ actions NOTIFY shortcutsChanged)  // what a shortcut can do
    // Mice and touchpads plugged in, with their own settings (refreshDevices()).
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(QVariant focusedWindow READ focusedWindow NOTIFY windowsChanged)
    Q_PROPERTY(QVariant focusedOutput READ focusedOutput NOTIFY outputsChanged)
    Q_PROPERTY(QVariant shownSecret READ shownSecret NOTIFY spacesChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    // {layouts: [{name: "German", code: "de"}], active: 0}
    Q_PROPERTY(QVariantMap keyboard READ keyboard NOTIFY keyboardChanged)

public:
    explicit Compositor(QObject* parent = nullptr);
    // The one connection, shared by the QML singleton and the state views.
    static Compositor* instance();

    QVariantList windows() const { return windows_; }  // most recently focused first
    QVariantList spaces() const { return spaces_; }
    QVariantMap keyboard() const { return keyboard_; }
    QVariantList outputs() const { return outputs_; }
    QVariantMap settings() const { return settings_; }
    QVariantList schema() const { return schema_; }
    QVariantList apps() const { return apps_; }
    QVariantList rules() const { return rules_; }
    QVariantList shortcuts() const { return shortcuts_; }
    QVariantList devices() const { return devices_; }
    QStringList actions() const { return actions_; }
    // Desktop entry ids pinned in the Dock, in order.
    QStringList dockPins() const;
    QVariant focusedWindow() const;
    QVariant focusedOutput() const;
    QVariant shownSecret() const;
    bool connected() const;

    // The numbered space shown on an output.
    Q_INVOKABLE int activeSpace(const QString& output) const;
    Q_INVOKABLE QVariantList spacesOn(const QString& output) const;
    Q_INVOKABLE QVariantList windowsOn(const QString& output, int space) const;
    // A fullscreen window is showing on the output: panels get out of the way.
    Q_INVOKABLE bool fullscreenOn(const QString& output) const;
    Q_INVOKABLE QVariant setting(const QString& key, const QVariant& fallback) const;
    // The shell's colours for appearance.style and appearance.accent:
    // m3Primary, m3Surface, ... as "#rrggbb" (palette.hpp).
    Q_INVOKABLE QVariantMap palette(bool light, const QString& accent) const;
    // The accent's own colour, for swatches; invalid for multicolour.
    Q_INVOKABLE QString accentColor(const QString& accent) const;
    Q_INVOKABLE QString accentLabel(const QString& accent) const;

    Q_INVOKABLE void switchSpace(int number);
    Q_INVOKABLE void toggleSecret(const QString& name);
    Q_INVOKABLE void focusWindow(int id);
    Q_INVOKABLE void closeWindow(int id);
    // Type into the focused text field (text-input-v3), as an input method
    // would; textNotInserted when no field takes it.
    Q_INVOKABLE void insertText(const QString& text);
    // "minimize", "maximize", "fullscreen", "pin", "to_space" ({number}), ...
    Q_INVOKABLE void windowRequest(int id, const QString& command, const QVariantMap& fields = {});
    Q_INVOKABLE void action(const QString& name, const QVariant& arg = {});
    Q_INVOKABLE void setKeyboardLayout(int index);
    Q_INVOKABLE void setSetting(const QString& key, const QVariant& value);
    Q_INVOKABLE void resetSetting(const QString& key);

    // Registry records. `fields` holds only what changes.
    Q_INVOKABLE void setApp(const QString& appId, const QVariantMap& fields);
    Q_INVOKABLE void forgetApp(const QString& appId);
    Q_INVOKABLE void setDock(const QStringList& appIds);
    Q_INVOKABLE void setPinned(const QString& appId, bool pinned);
    // A display's mode, scale, rotation, place or power: { width, height,
    // refresh, scale, transform, x, y, enabled }.
    Q_INVOKABLE void configureOutput(const QString& name, const QVariantMap& fields);
    Q_INVOKABLE void addRule(const QVariantMap& fields);
    Q_INVOKABLE void setRule(qint64 id, const QVariantMap& fields);
    Q_INVOKABLE void removeRule(qint64 id);
    Q_INVOKABLE void addShortcut(const QVariantMap& fields);
    Q_INVOKABLE void setShortcut(qint64 id, const QVariantMap& fields);
    Q_INVOKABLE void removeShortcut(qint64 id);
    Q_INVOKABLE void resetShortcuts();
    Q_INVOKABLE void refreshDevices();
    // A device's own settings: speed, acceleration, natural_scroll,
    // left_handed; an undefined/null value hands it back to the shared one.
    Q_INVOKABLE void setDevice(const QString& name, const QVariantMap& fields);

signals:
    // An app's global shortcut (bound through the portal) pressed or let go.
    void portalShortcut(const QString& app, const QString& id, bool pressed);
    void keyboardChanged();
    void windowsChanged();
    void spacesChanged();
    void outputsChanged();
    void settingsChanged();
    void schemaChanged();
    void appsChanged();
    void rulesChanged();
    void shortcutsChanged();
    void devicesChanged();
    // A registry change was refused: why, for the Settings app to say.
    void refused(const QString& why);
    void connectedChanged();
    // A shortcut asked the shell to show something ("launcher").
    // Right-click on a title bar (or an app asking): the window menu, at a
    // point on that output.
    void textNotInserted(const QString& text);
    void windowMenu(const QVariantMap& window, const QString& output, int x, int y);
    void shellAction(const QString& name);

private:
    using Reply = std::function<void(const QJsonValue&)>;
    using FullReply = std::function<void(const QJsonObject&)>;  // failures too
    void requestFull(QJsonObject req, FullReply reply);
    std::map<qint64, FullReply> fullPending_;

    void connectBoth();
    void request(QJsonObject req, Reply reply = {});
    void refreshAll();
    void readReplies();
    void readEvents();
    void applyEvent(const QJsonObject& event);
    void upsertWindow(const QVariantMap& window, bool toFront);

    QString path_;
    QLocalSocket requests_;
    QLocalSocket events_;
    QTimer reconnect_;
    QByteArray requestBuffer_, eventBuffer_;
    qint64 nextId_ = 1;
    std::map<qint64, Reply> pending_;

    QVariantList windows_, spaces_, outputs_;
    QVariantMap keyboard_;
    QVariantMap settings_;
    QVariantList schema_, apps_, rules_, shortcuts_, devices_;
    QStringList actions_;
    void refreshTable(const QString& table);
    void change(QJsonObject req);
};

} // namespace atrium
