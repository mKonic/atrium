#pragma once
// Which apps open what: the web browser (mimeapps.list, as every desktop
// and `xdg-open` read it) and the terminal (the terminal shortcut's
// setting, and xdg-terminals.list for xdg-terminal-exec). Each list is
// [{value, label, icon}] with a desktop id as the value.

#include <QObject>
#include <QVariantList>

namespace atrium {

class DefaultApps : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList browsers READ browsers NOTIFY changed)
    Q_PROPERTY(QString browser READ browser NOTIFY changed)
    Q_PROPERTY(QVariantList terminals READ terminals NOTIFY changed)
    Q_PROPERTY(QString terminal READ terminal NOTIFY changed)

public:
    explicit DefaultApps(QObject* parent = nullptr);

    QVariantList browsers() const;
    QString browser() const;
    QVariantList terminals() const;
    QString terminal() const;

    Q_INVOKABLE void setBrowser(const QString& id);
    Q_INVOKABLE void setTerminal(const QString& id);

signals:
    void changed();
};

} // namespace atrium
