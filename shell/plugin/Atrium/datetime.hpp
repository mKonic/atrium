#pragma once
// The clock's time zone and network time, through systemd's timedated (the
// polkit agent asks for a password where the system wants one).

#include <QObject>
#include <QStringList>
#include <QVariantList>

namespace atrium {

class DateTime : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString timezone READ timezone NOTIFY changed)        // "Europe/Berlin"
    Q_PROPERTY(QString timezoneLabel READ timezoneLabel NOTIFY changed)  // "Berlin (Europe)"
    Q_PROPERTY(bool ntp READ ntp NOTIFY changed)
    Q_PROPERTY(bool canNtp READ canNtp NOTIFY changed)

public:
    explicit DateTime(QObject* parent = nullptr);

    bool available() const { return available_; }
    QString timezone() const { return timezone_; }
    QString timezoneLabel() const { return label(timezone_); }
    bool ntp() const { return ntp_; }
    bool canNtp() const { return canNtp_; }

    // Time zones whose name has `query` in it: [{value, label}], by label.
    Q_INVOKABLE QVariantList findTimezones(const QString& query);
    Q_INVOKABLE void setTimezone(const QString& zone);
    Q_INVOKABLE void setNtp(bool on);

    static QString label(const QString& zone);

signals:
    void changed();
    void failed(const QString& why);

private slots:
    void propertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);

private:
    void load();
    void call(const QString& method, const QVariantList& args);

    bool available_ = false, ntp_ = false, canNtp_ = false;
    QString timezone_;
    QStringList zones_;  // asked for once, when first searched
};

} // namespace atrium
