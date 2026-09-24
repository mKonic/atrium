#include "region.hpp"

#include <QCollator>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QLocale>
#include <QProcess>

#include <algorithm>

namespace atrium {

namespace {

const QString kService = QStringLiteral("org.freedesktop.locale1");
const QString kPath = QStringLiteral("/org/freedesktop/locale1");
const QString kProps = QStringLiteral("org.freedesktop.DBus.Properties");

// What formats cover; the language keeps LANG (and so messages).
const QStringList kFormats = {"LC_TIME", "LC_NUMERIC", "LC_MONETARY", "LC_MEASUREMENT", "LC_PAPER"};

} // namespace

Region::Region(QObject* parent) : QObject(parent) {
    QDBusConnection::systemBus().connect(kService, kPath, kProps, "PropertiesChanged", this,
                                         SLOT(propertiesChanged(QString, QVariantMap, QStringList)));
    load();
}

void Region::load() {
    QDBusMessage m = QDBusMessage::createMethodCall(kService, kPath, kProps, "Get");
    m << kService << QStringLiteral("Locale");
    auto* w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(m), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
        w->deleteLater();
        QDBusPendingReply<QDBusVariant> r = *w;
        available_ = !r.isError();
        if (available_)
            locale_ = r.value().variant().toStringList();
        emit changed();
    });
}

void Region::propertiesChanged(const QString& interface, const QVariantMap&, const QStringList&) {
    if (interface == kService)
        load();
}

QString Region::value(const QString& key) const {
    for (const QString& a : locale_)
        if (a.startsWith(key + "="))
            return a.mid(key.size() + 1);
    return {};
}

QString Region::formats() const {
    const QString time = value("LC_TIME");
    return time.isEmpty() ? language() : time;
}

QString Region::label(const QString& locale) {
    const QString name = locale.section('.', 0, 0).section('@', 0, 0);
    if (name == "C" || name == "POSIX")
        return QStringLiteral("None (C)");
    const QLocale l(name);
    if (l.language() == QLocale::C)
        return locale;
    QString text = QLocale::languageToString(l.language());
    if (name.contains('_'))
        text += " (" + QLocale::territoryToString(l.territory()) + ")";
    // In its own words too, when they differ: "German (Germany) · Deutsch (Deutschland)".
    const QString native = l.nativeLanguageName() + (name.contains('_') ? " (" + l.nativeTerritoryName() + ")" : "");
    if (l.language() != QLocale::English && !l.nativeLanguageName().isEmpty() &&
        native.compare(text, Qt::CaseInsensitive) != 0)
        text += " · " + native;
    return text;
}

QVariantList Region::locales() const {
    if (!locales_.isEmpty())
        return locales_;
    QProcess p;
    p.start("localectl", {"list-locales", "--no-pager"});
    p.waitForFinished(3000);
    for (const QString& line : QString::fromUtf8(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts))
        locales_.append(QVariantMap{{"value", line.trimmed()}, {"label", label(line.trimmed())}});
    QCollator order;
    std::sort(locales_.begin(), locales_.end(), [&](const QVariant& a, const QVariant& b) {
        return order.compare(a.toMap().value("label").toString(), b.toMap().value("label").toString()) < 0;
    });
    return locales_;
}

QString Region::sample() const {
    const QLocale l(formats().section('.', 0, 0));
    const QDateTime now = QDateTime::currentDateTime();
    return QStringList{l.toString(now.date(), QLocale::LongFormat), l.toString(now.time(), QLocale::ShortFormat),
                       l.toString(1234567.89, 'f', 2), l.toCurrencyString(12.5)}
        .join(" · ");
}

void Region::setLocale(const QStringList& assignments) {
    QDBusMessage m = QDBusMessage::createMethodCall(kService, kPath, kService, "SetLocale");
    m << assignments << true;
    // Or polkit refuses outright instead of asking for a password.
    m.setInteractiveAuthorizationAllowed(true);
    auto* w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(m, 5 * 60 * 1000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
        w->deleteLater();
        QDBusPendingReply<> r = *w;
        if (r.isError())
            emit failed(r.error().message());
        load();
    });
}

void Region::setLanguage(const QString& locale) {
    // The formats stay what they were, even when they followed the language.
    QStringList next = {"LANG=" + locale};
    const QString keep = formats();
    for (const QString& key : kFormats)
        next.append(key + "=" + (value(key).isEmpty() ? keep : value(key)));
    setLocale(next);
}

void Region::setFormats(const QString& locale) {
    QStringList next = {"LANG=" + language()};
    for (const QString& key : kFormats)
        next.append(key + "=" + locale);
    setLocale(next);
}

} // namespace atrium
