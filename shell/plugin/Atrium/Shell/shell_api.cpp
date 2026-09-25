#include "shell_api.hpp"

#include "desktop_entries.hpp"
#include "screens.hpp"

#include <cstdio>
#include <unistd.h>
#include <QFile>
#include <QFileInfo>
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

void ShellApi::printLine(const QString& text) const {
    const QByteArray line = text.toUtf8() + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

namespace {

QStringList cmdline_of(pid_t pid) {
    QFile f(QStringLiteral("/proc/%1/cmdline").arg(pid));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QStringList args;
    for (const QByteArray& a : f.readAll().split('\0'))
        if (!a.isEmpty())
            args.append(QString::fromLocal8Bit(a));
    return args;
}

pid_t parent_of(pid_t pid) {
    QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    // pid (comm) state ppid ...: comm may hold spaces and parentheses.
    const QByteArray stat = f.readAll();
    const qsizetype close = stat.lastIndexOf(')');
    const QList<QByteArray> rest = stat.mid(close + 2).split(' ');
    return rest.size() > 1 ? pid_t(rest[1].toInt()) : 0;
}

// sudo's own options before the command, and which of them take a value.
QStringList sudo_command(QStringList args) {
    static const QStringList with_value = {"-u", "-g", "-p", "-C", "-h", "-r", "-t", "-U", "-D", "-R", "-T"};
    args.removeFirst();
    while (!args.isEmpty() && args.first().startsWith('-')) {
        const QString opt = args.takeFirst();
        if (opt == "--")
            break;
        if (with_value.contains(opt) && !args.isEmpty())
            args.removeFirst();
    }
    return args;
}

} // namespace

QVariantMap ShellApi::askingProcess() const {
    QVariantMap out;
    pid_t pid = getppid();
    const QStringList asker = cmdline_of(pid);
    if (!asker.isEmpty()) {
        const QString exe = QFileInfo(asker.first()).fileName();
        out["command"] = (exe == "sudo" ? sudo_command(asker) : asker).join(' ');
    }
    // Up to the app it came from, past the shells in between.
    for (int depth = 0; depth < 16 && pid > 1; ++depth) {
        pid = parent_of(pid);
        const QStringList args = cmdline_of(pid);
        if (args.isEmpty())
            continue;
        const QString name = QFileInfo(args.first()).fileName();
        if (DesktopEntry* e = DesktopEntries::instance()->heuristicLookup(name)) {
            out["app"] = e->name();
            out["icon"] = e->icon();
            break;
        }
    }
    return out;
}

} // namespace atrium::shell
