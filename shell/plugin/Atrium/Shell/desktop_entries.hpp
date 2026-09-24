#pragma once
// Installed applications for QML: `DesktopEntries.byId("firefox")`,
// `DesktopEntries.applications.values`. Rescanned when an applications
// directory changes (a package installed or removed).

#include "desktop_entry_core.hpp"

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>

namespace atrium::shell {

class DesktopAction : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString id READ id CONSTANT)
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(QString icon READ icon CONSTANT)
    Q_PROPERTY(QString execString READ execString CONSTANT)
    Q_PROPERTY(QStringList command READ command CONSTANT)

public:
    DesktopAction(const desktop_entry::Action& a, class DesktopEntry* entry);

    QString id() const { return QString::fromStdString(a_.id); }
    QString name() const { return QString::fromStdString(a_.name); }
    QString icon() const { return QString::fromStdString(a_.icon); }
    QString execString() const { return QString::fromStdString(a_.exec); }
    QStringList command() const;
    Q_INVOKABLE void execute() const;

private:
    desktop_entry::Action a_;
    DesktopEntry* entry_;
};

class DesktopEntry : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString id READ id CONSTANT)
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(QString genericName READ genericName CONSTANT)
    Q_PROPERTY(QString comment READ comment CONSTANT)
    Q_PROPERTY(QString icon READ icon CONSTANT)
    Q_PROPERTY(QString execString READ execString CONSTANT)
    Q_PROPERTY(QStringList command READ command CONSTANT)
    Q_PROPERTY(QString workingDirectory READ workingDirectory CONSTANT)
    Q_PROPERTY(QString startupClass READ startupClass CONSTANT)
    Q_PROPERTY(QStringList keywords READ keywords CONSTANT)
    Q_PROPERTY(QStringList categories READ categories CONSTANT)
    Q_PROPERTY(bool noDisplay READ noDisplay CONSTANT)
    Q_PROPERTY(bool runInTerminal READ runInTerminal CONSTANT)
    Q_PROPERTY(QList<QObject*> actions READ actions CONSTANT)

public:
    DesktopEntry(QString id, QString file, desktop_entry::Entry e, bool shown, QObject* parent);

    QString id() const { return id_; }
    QString file() const { return file_; }
    QString name() const { return QString::fromStdString(e_.name); }
    QString genericName() const { return QString::fromStdString(e_.generic_name); }
    QString comment() const { return QString::fromStdString(e_.comment); }
    QString icon() const { return QString::fromStdString(e_.icon); }
    QString execString() const { return QString::fromStdString(e_.exec); }
    QStringList command() const { return argv(e_.exec); }
    QString workingDirectory() const { return QString::fromStdString(e_.path); }
    QString startupClass() const { return QString::fromStdString(e_.startup_wm_class); }
    QStringList keywords() const;
    QStringList categories() const;
    bool noDisplay() const { return !shown_; }
    bool runInTerminal() const { return e_.terminal; }
    QList<QObject*> actions() const { return actions_; }

    Q_INVOKABLE void execute() const;

    QStringList argv(const std::string& exec) const;
    void launch(QStringList argv) const;

private:
    QString id_;
    QString file_;
    desktop_entry::Entry e_;
    bool shown_;
    QList<QObject*> actions_;
};

// `applications.values`: every application, as one list that notifies.
class EntryList : public QObject {
    Q_OBJECT
    Q_PROPERTY(QList<QObject*> values READ values NOTIFY valuesChanged)

public:
    using QObject::QObject;
    QList<QObject*> values() const { return values_; }
    void set(QList<QObject*> v) {
        values_ = std::move(v);
        emit valuesChanged();
    }

signals:
    void valuesChanged();

private:
    QList<QObject*> values_;
};

class DesktopEntries : public QObject {
    Q_OBJECT
    Q_PROPERTY(atrium::shell::EntryList* applications READ applications NOTIFY applicationsChanged)
    // How apps that want a terminal are started: the terminal's command, to
    // which "-e" and the app's command are added. xdg-terminal-exec, when
    // installed, is used instead.
    Q_PROPERTY(QString terminal READ terminal WRITE setTerminal NOTIFY terminalChanged)

public:
    static DesktopEntries* instance();

    EntryList* applications() { return &list_; }
    QString terminal() const { return terminal_; }
    void setTerminal(const QString& t);

    Q_INVOKABLE atrium::shell::DesktopEntry* byId(const QString& id) const;
    // For an app id or window class that isn't exactly a desktop file's id:
    // case-insensitive id, StartupWMClass, the last part of a reverse-DNS id,
    // then the name.
    Q_INVOKABLE atrium::shell::DesktopEntry* heuristicLookup(const QString& name) const;

signals:
    void applicationsChanged();
    void terminalChanged();

private:
    DesktopEntries();
    void scan();
    QStringList directories() const;

    EntryList list_;
    QHash<QString, DesktopEntry*> byId_;
    QString terminal_ = QStringLiteral("foot");
    QFileSystemWatcher watcher_;
    QTimer rescan_;
};

} // namespace atrium::shell
