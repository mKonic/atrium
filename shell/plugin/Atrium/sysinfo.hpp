#pragma once
// What the About card says about this computer: `SystemInfo.cpu`, ... Read
// once; uptime on asking.

#include <QObject>
#include <QStringList>

namespace atrium {

class SystemInfo : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString osName READ osName CONSTANT)
    Q_PROPERTY(QString logo READ logo CONSTANT)  // icon name, from os-release LOGO
    Q_PROPERTY(QString hostname READ hostname CONSTANT)
    Q_PROPERTY(QString kernel READ kernel CONSTANT)
    Q_PROPERTY(QString cpu READ cpu CONSTANT)
    Q_PROPERTY(QStringList gpus READ gpus CONSTANT)  // discrete first
    Q_PROPERTY(QString memory READ memory CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)  // atrium's
    Q_PROPERTY(QString user READ user CONSTANT)

public:
    explicit SystemInfo(QObject* parent = nullptr);

    QString osName() const { return osName_; }
    QString logo() const { return logo_; }
    QString hostname() const { return hostname_; }
    QString kernel() const { return kernel_; }
    QString cpu() const { return cpu_; }
    QStringList gpus() const { return gpus_; }
    QString memory() const { return memory_; }
    QString version() const;
    QString user() const { return user_; }

    // "3 days, 4 hours"
    Q_INVOKABLE QString uptime() const;

private:
    QString osName_, logo_, hostname_, kernel_, cpu_, memory_, user_;
    QStringList gpus_;
};

} // namespace atrium
