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
    Q_INVOKABLE void execDetached(const QStringList& command) const;
    Q_INVOKABLE QString env(const QString& name) const;

signals:
    void screensChanged();
    void clipboardTextChanged();
};

} // namespace atrium::shell
