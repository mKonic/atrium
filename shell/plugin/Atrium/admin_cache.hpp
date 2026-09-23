#pragma once
// The administrator password, for five minutes after it last worked, so a
// run of admin actions asks once (as sudo's timestamp does). Held here, not
// in QML, and overwritten when it expires. `AdminCache` singleton.

#include <QObject>
#include <QTimer>

namespace atrium {

class AdminCache : public QObject {
    Q_OBJECT

public:
    static constexpr int kKeepMs = 5 * 60 * 1000;

    explicit AdminCache(QObject* parent = nullptr);
    ~AdminCache() override;

    // A password just typed for user `uid`; kept only if commit() follows
    // (the authentication succeeded).
    Q_INVOKABLE void stage(uint uid, const QString& password);
    Q_INVOKABLE void commit();
    // The password for `uid` if still fresh, else "".
    Q_INVOKABLE QString recall(uint uid) const;
    Q_INVOKABLE void forget();

private:
    QString password_, staged_;
    uint uid_ = 0, stagedUid_ = 0;
    QTimer expiry_;
};

} // namespace atrium
