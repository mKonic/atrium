#pragma once
// The people who use this machine, from AccountsService (the system's
// record of users: name, picture, whether they're an admin), for the
// Settings app's Users & Groups page. Changes go back through it, which
// asks polkit when they need an admin.

#include <QObject>
#include <QVariantList>

namespace atrium {

class Accounts : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QVariantMap me READ me NOTIFY changed)           // { userName, realName, initials, icon, admin }
    Q_PROPERTY(QVariantList others READ others NOTIFY changed)  // the other people, the same way

public:
    explicit Accounts(QObject* parent = nullptr);

    bool available() const { return available_; }
    QVariantMap me() const { return me_; }
    QVariantList others() const { return others_; }

    Q_INVOKABLE void setRealName(const QString& name);
    // A picture file (a path or file:// URL); AccountsService keeps its own copy.
    Q_INVOKABLE void setPicture(const QString& file);
    Q_INVOKABLE void refresh();

signals:
    void changed();
    void failed(const QString& why);

private slots:
    void userChanged();

private:
    QVariantMap describe(const QString& path) const;
    void call(const QString& method, const QVariant& arg);

    bool available_ = false;
    QString myPath_;
    QVariantMap me_;
    QVariantList others_;
};

} // namespace atrium
