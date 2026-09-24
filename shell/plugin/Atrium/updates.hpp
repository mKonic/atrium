#pragma once
// Software updates through PackageKit (any distro it has a backend for;
// alpm on Arch). The desktop shell checks at login and every six hours;
// installing goes by the system's polkit rules (Arch lets an active local
// user update without a password; elsewhere the polkit agent asks).
//
//   packages: [{id, name, version, repo, summary, security}], by name

#include <QDBusObjectPath>
#include <QDateTime>
#include <QObject>
#include <QTimer>
#include <QVariantList>

#include <functional>

namespace atrium {

class Updates : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)      // PackageKit is there
    Q_PROPERTY(bool checking READ checking NOTIFY changed)
    Q_PROPERTY(bool known READ known NOTIFY changed)  // PackageKit has said what's newer
    Q_PROPERTY(bool installing READ installing NOTIFY changed)
    Q_PROPERTY(QVariantList packages READ packages NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)       // "3 updates, 1 for security"
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)  // 0..100, while installing
    Q_PROPERTY(QString doing READ doing NOTIFY progressChanged)    // "Downloading firefox"
    Q_PROPERTY(QString lastChecked READ lastChecked NOTIFY changed)  // "Today at 14:05", "" never
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool restartNeeded READ restartNeeded NOTIFY changed)

public:
    explicit Updates(QObject* parent = nullptr);

    bool available() const { return available_; }
    bool checking() const { return checking_; }
    bool known() const { return known_; }
    bool installing() const { return installing_; }
    QVariantList packages() const { return packages_; }
    int count() const { return int(packages_.size()); }
    QString summary() const;
    int progress() const { return progress_; }
    QString doing() const { return doing_; }
    QString lastChecked() const;
    QString error() const { return error_; }
    bool restartNeeded() const { return restartNeeded_; }

    // Refresh the package lists, then see what's newer.
    Q_INVOKABLE void check();
    // Install all of them.
    Q_INVOKABLE void install();
    Q_INVOKABLE void cancel();

signals:
    void changed();
    void progressChanged();

private slots:
    // A transaction's signals (sender path tells which).
    void onPackage(uint info, const QString& id, const QString& summary);
    void onFinished(uint exit, uint runtime);
    void onErrorCode(uint code, const QString& details);
    void onRequireRestart(uint type, const QString& id);
    void onProperties(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);
    void list();  // what's newer, as PackageKit already knows (it says when that changes)

private:
    enum class Step { None, Refresh, List, Install };
    // Starts `method` on a new transaction; false when PackageKit isn't there.
    bool begin(Step step, const QString& method, const QVariantList& args);
    void end();
    void readLastChecked();

    bool known_ = false;
    bool available_ = false, checking_ = false, installing_ = false, restartNeeded_ = false;
    QVariantList packages_, found_;
    int progress_ = 0;
    QString doing_, error_;
    QDateTime lastChecked_;
    Step step_ = Step::None;
    QString transaction_;
    QTimer schedule_;
};

} // namespace atrium
