#include "accounts.hpp"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QFileInfo>
#include <QUrl>

#include <unistd.h>

namespace atrium {

namespace {

const QString kService = "org.freedesktop.Accounts";
const QString kUser = "org.freedesktop.Accounts.User";

} // namespace

Accounts::Accounts(QObject* parent) : QObject(parent) {
    refresh();
}

QVariantMap Accounts::describe(const QString& path) const {
    QDBusInterface user(kService, path, kUser, QDBusConnection::systemBus());
    if (!user.isValid())
        return {};
    const QString icon = user.property("IconFile").toString();
    const QString name = user.property("RealName").toString();
    // Up to two initials, from the full name or else the account name.
    QString initials;
    for (const QString& word : (name.isEmpty() ? user.property("UserName").toString() : name).split(' ', Qt::SkipEmptyParts))
        if (initials.size() < 2)
            initials += word.front().toUpper();
    return {
        {"initials", initials.isEmpty() ? QString("?") : initials},
        {"userName", user.property("UserName").toString()},
        {"realName", user.property("RealName").toString()},
        // A picture only if there is one: the page draws initials otherwise.
        {"icon", !icon.isEmpty() && QFileInfo(icon).isFile() && QFileInfo(icon).size() > 0
                     ? QUrl::fromLocalFile(icon).toString() + "?" + QString::number(QFileInfo(icon).lastModified().toSecsSinceEpoch())
                     : QString()},
        {"admin", user.property("AccountType").toInt() == 1},
        {"system", user.property("SystemAccount").toBool()},
    };
}

void Accounts::refresh() {
    QDBusInterface accounts(kService, "/org/freedesktop/Accounts", kService, QDBusConnection::systemBus());
    QDBusReply<QDBusObjectPath> mine = accounts.call("FindUserById", qlonglong(getuid()));
    available_ = mine.isValid();
    if (!available_) {
        emit changed();
        return;
    }
    if (myPath_ != mine.value().path()) {
        if (!myPath_.isEmpty())
            QDBusConnection::systemBus().disconnect(kService, myPath_, kUser, "Changed", this, SLOT(userChanged()));
        myPath_ = mine.value().path();
        // AccountsService says "Changed" rather than PropertiesChanged.
        QDBusConnection::systemBus().connect(kService, myPath_, kUser, "Changed", this, SLOT(userChanged()));
    }
    me_ = describe(myPath_);

    others_.clear();
    QDBusReply<QList<QDBusObjectPath>> all = accounts.call("ListCachedUsers");
    if (all.isValid())
        for (const QDBusObjectPath& p : all.value()) {
            if (p.path() == myPath_)
                continue;
            const QVariantMap u = describe(p.path());
            if (!u.isEmpty() && !u.value("system").toBool())
                others_.push_back(u);
        }
    emit changed();
}

void Accounts::userChanged() {
    refresh();
}

void Accounts::call(const QString& method, const QVariant& arg) {
    if (myPath_.isEmpty())
        return;
    QDBusInterface user(kService, myPath_, kUser, QDBusConnection::systemBus());
    // Interactive: polkit may ask for a password, so don't block on it.
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, myPath_, kUser, method);
    msg.setArguments({arg});
    msg.setInteractiveAuthorizationAllowed(true);
    auto* watch = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg, 120000), this);
    connect(watch, &QDBusPendingCallWatcher::finished, this, [this, watch] {
        if (watch->isError())
            emit failed(watch->error().message());
        watch->deleteLater();
        refresh();
    });
}

void Accounts::setRealName(const QString& name) {
    if (name.trimmed() != me_.value("realName").toString())
        call("SetRealName", name.trimmed());
}

void Accounts::setPicture(const QString& file) {
    const QString path = file.startsWith("file:") ? QUrl(file).toLocalFile() : file;
    if (QFileInfo(path).isFile())
        call("SetIconFile", path);
}

} // namespace atrium
