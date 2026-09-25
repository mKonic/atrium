#pragma once
// The `Shell` singleton: the screens, where the shell's files are, and the
// few process-level things QML can't do itself.

#include <QObject>
#include <QStringList>
#include <QVariant>

#include "screens.hpp"

namespace atrium::shell {

class ShellApi : public QObject {
    Q_OBJECT
    Q_PROPERTY(QList<ShellScreen*> screens READ screens NOTIFY screensChanged)
    Q_PROPERTY(QString shellDir READ shellDir CONSTANT)
    Q_PROPERTY(QString clipboardText READ clipboardText WRITE setClipboardText NOTIFY clipboardTextChanged)

public:
    explicit ShellApi(QObject* parent = nullptr);

    QList<ShellScreen*> screens() const;
    QString shellDir() const;
    QString clipboardText() const;
    void setClipboardText(const QString& text);

    // An icon from the theme as an image source, "image://icon/name". With a
    // string `fallback`, that icon when `name` has none; with `true`, "" when
    // the theme has no such icon; otherwise the missing-icon image.
    Q_INVOKABLE QString iconPath(const QString& name, const QVariant& fallback = {}) const;
    // The screen called `name`, else the first (where a panel goes when the
    // focused output isn't known yet).
    Q_INVOKABLE atrium::shell::ShellScreen* screen(const QString& name) const;
    Q_INVOKABLE void execDetached(const QStringList& command) const;
    // Another of the shell's files as its own app ("settings.qml"), unless
    // it already runs; `env` is added to its environment.
    Q_INVOKABLE void launch(const QString& file, const QVariantMap& env = {}) const;
    Q_INVOKABLE QString env(const QString& name) const;
    // A line on standard output (the askpass answers sudo so).
    Q_INVOKABLE void printLine(const QString& text) const;
    // Who asks the askpass for a password: { command } is what sudo will run
    // (or ssh's own command line), { app, icon } the first app with a
    // desktop entry up the process tree (the terminal it was typed in).
    Q_INVOKABLE QVariantMap askingProcess() const;

signals:
    void screensChanged();
    void clipboardTextChanged();
};

} // namespace atrium::shell
