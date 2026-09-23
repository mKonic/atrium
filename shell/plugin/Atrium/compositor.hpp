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
    Q_PROPERTY(QVariant focusedWindow READ focusedWindow NOTIFY windowsChanged)
    Q_PROPERTY(QVariant focusedOutput READ focusedOutput NOTIFY outputsChanged)
    Q_PROPERTY(QVariant shownSecret READ shownSecret NOTIFY spacesChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)

public:
    explicit Compositor(QObject* parent = nullptr);
    // The one connection, shared by the QML singleton and the state views.
    static Compositor* instance();

    QVariantList windows() const { return windows_; }  // most recently focused first
    QVariantList spaces() const { return spaces_; }
    QVariantList outputs() const { return outputs_; }
    QVariantMap settings() const { return settings_; }
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

    Q_INVOKABLE void switchSpace(int number);
    Q_INVOKABLE void toggleSecret(const QString& name);
    Q_INVOKABLE void focusWindow(int id);
    Q_INVOKABLE void closeWindow(int id);
    Q_INVOKABLE void action(const QString& name, const QVariant& arg = {});
    Q_INVOKABLE void setSetting(const QString& key, const QVariant& value);

signals:
    void windowsChanged();
    void spacesChanged();
    void outputsChanged();
    void settingsChanged();
    void connectedChanged();
    // A shortcut asked the shell to show something ("launcher").
    void shellAction(const QString& name);

private:
    using Reply = std::function<void(const QJsonValue&)>;

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
    QVariantMap settings_;
};

} // namespace atrium
