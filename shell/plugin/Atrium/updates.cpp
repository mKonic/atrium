#include "updates.hpp"

#include "updates_core.hpp"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QGuiApplication>
#include <QLocale>

#include <algorithm>

namespace atrium {

namespace {

const QString kService = QStringLiteral("org.freedesktop.PackageKit");
const QString kPath = QStringLiteral("/org/freedesktop/PackageKit");
const QString kTransaction = QStringLiteral("org.freedesktop.PackageKit.Transaction");

QDBusConnection bus() {
    return QDBusConnection::systemBus();
}

QDBusMessage call(const QString& path, const QString& interface, const QString& method,
                  const QVariantList& args = {}) {
    QDBusMessage m = QDBusMessage::createMethodCall(kService, path, interface, method);
    m.setArguments(args);
    return m;
}

} // namespace

Updates::Updates(QObject* parent) : QObject(parent) {
    QDBusConnectionInterface* iface = bus().interface();
    available_ = iface && (iface->isServiceRegistered(kService).value() ||
                           iface->activatableServiceNames().value().contains(kService));
    if (!available_)
        return;
    bus().connect(kService, kPath, kService, "UpdatesChanged", this, SLOT(list()));
    readLastChecked();
    list();
    // Only the desktop shell checks by itself: a minute after login, then
    // every six hours.
    if (QGuiApplication::desktopFileName() == "atrium-shell") {
        connect(&schedule_, &QTimer::timeout, this, [this] {
            schedule_.setInterval(6 * 60 * 60 * 1000);
            if (!installing_ && !checking_)
                check();
        });
        schedule_.start(60 * 1000);
    }
}

QString Updates::summary() const {
    int security = 0;
    for (const QVariant& p : packages_)
        security += p.toMap().value("security").toBool();
    return QString::fromStdString(updates::summary(count(), security));
}

QString Updates::lastChecked() const {
    if (!lastChecked_.isValid())
        return {};
    const QDate day = lastChecked_.date(), today = QDate::currentDate();
    const QString time = QLocale().toString(lastChecked_.time(), QLocale::ShortFormat);
    if (day == today)
        return "Today at " + time;
    if (day == today.addDays(-1))
        return "Yesterday at " + time;
    return QLocale().toString(day, QLocale::ShortFormat) + " at " + time;
}

void Updates::readLastChecked() {
    const QDBusMessage r = bus().call(call(kPath, kService, "GetTimeSinceAction", {uint(updates::pk::kRoleRefreshCache)}));
    if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty())
        return;
    const uint seconds = r.arguments().first().toUInt();
    // PackageKit says "never" with the largest number it has.
    if (seconds > 0 && seconds < 365u * 24 * 60 * 60)
        lastChecked_ = QDateTime::currentDateTime().addSecs(-qint64(seconds));
}

bool Updates::begin(Step step, const QString& method, const QVariantList& args) {
    const QDBusMessage created = bus().call(call(kPath, kService, "CreateTransaction"));
    if (created.type() != QDBusMessage::ReplyMessage || created.arguments().isEmpty()) {
        error_ = "PackageKit didn't answer.";
        emit changed();
        return false;
    }
    transaction_ = created.arguments().first().value<QDBusObjectPath>().path();
    step_ = step;
    for (const char* signal : {"Package", "Finished", "ErrorCode", "RequireRestart"}) {
        const char* slot = !qstrcmp(signal, "Package") ? SLOT(onPackage(uint, QString, QString))
                         : !qstrcmp(signal, "Finished") ? SLOT(onFinished(uint, uint))
                         : !qstrcmp(signal, "ErrorCode") ? SLOT(onErrorCode(uint, QString))
                                                         : SLOT(onRequireRestart(uint, QString));
        bus().connect(kService, transaction_, kTransaction, signal, this, slot);
    }
    bus().connect(kService, transaction_, "org.freedesktop.DBus.Properties", "PropertiesChanged", this,
                  SLOT(onProperties(QString, QVariantMap, QStringList)));
    // A password may be asked for, in the language in use.
    bus().call(call(transaction_, kTransaction, "SetHints",
                    {QStringList{"interactive=true", "locale=" + QLocale().name() + ".UTF-8"}}));
    QDBusMessage m = call(transaction_, kTransaction, method, args);
    m.setInteractiveAuthorizationAllowed(true);
    bus().asyncCall(m);
    return true;
}

void Updates::end() {
    for (const char* signal : {"Package", "Finished", "ErrorCode", "RequireRestart"})
        bus().disconnect(kService, transaction_, kTransaction, signal, this, nullptr);
    bus().disconnect(kService, transaction_, "org.freedesktop.DBus.Properties", "PropertiesChanged", this, nullptr);
    transaction_.clear();
    step_ = Step::None;
}

void Updates::check() {
    if (!available_ || step_ != Step::None)
        return;
    checking_ = true;
    error_.clear();
    emit changed();
    if (!begin(Step::Refresh, "RefreshCache", {false})) {
        checking_ = false;
        emit changed();
    }
}

void Updates::list() {
    if (!available_ || step_ != Step::None)
        return;
    found_.clear();
    begin(Step::List, "GetUpdates", {qulonglong(updates::pk::kFilterNone)});
}

void Updates::install() {
    if (!available_ || step_ != Step::None || packages_.isEmpty())
        return;
    QStringList ids;
    for (const QVariant& p : packages_)
        ids << p.toMap().value("id").toString();
    installing_ = true;
    progress_ = 0;
    doing_.clear();
    error_.clear();
    emit changed();
    emit progressChanged();
    if (!begin(Step::Install, "UpdatePackages", {qulonglong(updates::pk::kOnlyTrusted), ids})) {
        installing_ = false;
        emit changed();
    }
}

void Updates::cancel() {
    if (!transaction_.isEmpty())
        bus().asyncCall(call(transaction_, kTransaction, "Cancel"));
}

void Updates::onPackage(uint info, const QString& id, const QString& summary) {
    const updates::PackageId p = updates::parse_package_id(id.toStdString());
    if (p.name.empty())
        return;
    if (step_ == Step::List) {
        found_.append(QVariantMap{
            {"id", id},
            {"name", QString::fromStdString(p.name)},
            {"version", QString::fromStdString(p.version)},
            {"repo", QString::fromStdString(p.repo)},
            {"summary", summary},
            {"security", info == updates::pk::kInfoSecurity},
        });
    } else if (step_ == Step::Install) {
        const std::string what = updates::doing(info);
        if (!what.empty()) {
            doing_ = QString::fromStdString(what) + " " + QString::fromStdString(p.name);
            emit progressChanged();
        }
        if (updates::needs_restart(p.name) && info == updates::pk::kInfoFinished && !restartNeeded_) {
            restartNeeded_ = true;
            emit changed();
        }
    }
}

void Updates::onRequireRestart(uint type, const QString&) {
    using namespace updates::pk;
    if (type == kRestartSession || type == kRestartSystem || type == kRestartSecuritySession ||
        type == kRestartSecuritySystem) {
        restartNeeded_ = true;
        emit changed();
    }
}

void Updates::onErrorCode(uint, const QString& details) {
    error_ = details.trimmed();
    emit changed();
}

void Updates::onProperties(const QString&, const QVariantMap& changed, const QStringList&) {
    if (step_ != Step::Install || !changed.contains("Percentage"))
        return;
    const uint p = changed.value("Percentage").toUInt();
    if (p <= 100) {  // 101: not known
        progress_ = int(p);
        emit progressChanged();
    }
}

void Updates::onFinished(uint exit, uint) {
    const Step step = step_;
    end();
    switch (step) {
    case Step::Refresh:
        readLastChecked();
        list();  // whether or not the refresh worked: what's known
        return;
    case Step::List:
        std::ranges::sort(found_, [](const QVariant& a, const QVariant& b) {
            return a.toMap().value("name").toString() < b.toMap().value("name").toString();
        });
        packages_ = found_;
        found_.clear();
        checking_ = false;
        emit changed();
        return;
    case Step::Install:
        installing_ = false;
        doing_.clear();
        if (exit == updates::pk::kExitSuccess)
            progress_ = 100;
        else if (exit == updates::pk::kExitCancelled && error_.isEmpty())
            error_ = "Cancelled.";
        emit progressChanged();
        emit changed();
        list();
        return;
    case Step::None:
        return;
    }
}

} // namespace atrium
