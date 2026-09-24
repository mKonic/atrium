#include "shell_api.hpp"

#include "screens.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QIcon>
#include <QCoreApplication>
#include <QProcess>

namespace atrium::shell {

ShellApi::ShellApi(QObject* parent) : QObject(parent) {
    connect(Screens::instance(), &Screens::changed, this, &ShellApi::screensChanged);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &ShellApi::clipboardTextChanged);
}

QList<ShellScreen*> ShellApi::screens() const {
    return Screens::instance()->list();
}

ShellScreen* ShellApi::screen(const QString& name) const {
    const QList<ShellScreen*> list = Screens::instance()->list();
    for (ShellScreen* s : list)
        if (s->name() == name)
            return s;
    return list.isEmpty() ? nullptr : list.first();
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

void ShellApi::launch(const QString& file, const QVariantMap& env) const {
    QProcess p;
    p.setProgram(QCoreApplication::applicationFilePath());
    p.setArguments({QStringLiteral("--no-duplicate"), shellDir() + "/" + file});
    QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
    for (auto it = env.begin(); it != env.end(); ++it)
        e.insert(it.key(), it.value().toString());
    p.setProcessEnvironment(e);
    p.startDetached();
}

QString ShellApi::env(const QString& name) const {
    return qEnvironmentVariable(name.toUtf8().constData());
}

} // namespace atrium::shell
