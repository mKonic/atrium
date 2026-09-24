#include "shell_api.hpp"

#include "screens.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QIcon>
#include <QProcess>

namespace atrium::shell {

ShellApi::ShellApi(QObject* parent) : QObject(parent) {
    connect(Screens::instance(), &Screens::changed, this, &ShellApi::screensChanged);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &ShellApi::clipboardTextChanged);
}

QList<ShellScreen*> ShellApi::screens() const {
    return Screens::instance()->list();
}

QString ShellApi::shellDir() const {
    return qApp->property("atriumShellDir").toString();
}

QString ShellApi::clipboardText() const {
    return QGuiApplication::clipboard()->text();
}

void ShellApi::setClipboardText(const QString& text) {
    QGuiApplication::clipboard()->setText(text);
}

QString ShellApi::iconPath(const QString& name, const QVariant& fallback) const {
    // Absolute paths and URLs are already sources.
    if (name.startsWith('/'))
        return "file://" + name;
    if (name.contains("://"))
        return name;
    if (!name.isEmpty() && QIcon::hasThemeIcon(name))
        return "image://icon/" + name;
    if (fallback.typeId() == QMetaType::QString && !fallback.toString().isEmpty())
        return "image://icon/" + fallback.toString();
    if (fallback.typeId() == QMetaType::Bool && fallback.toBool())
        return {};
    return "image://icon/" + (name.isEmpty() ? QStringLiteral("image-missing") : name);
}

void ShellApi::execDetached(const QStringList& command) const {
    if (command.isEmpty())
        return;
    QProcess::startDetached(command.first(), command.mid(1));
}

QString ShellApi::env(const QString& name) const {
    return qEnvironmentVariable(name.toUtf8().constData());
}

} // namespace atrium::shell
