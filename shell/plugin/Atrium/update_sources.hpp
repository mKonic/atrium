#pragma once
// Software Update's other sources, beside PackageKit (updates.hpp):
//
//   PacmanUpdates  the system through pacman itself, where PackageKit isn't
//                  installed: checkupdates to look (no root needed), pkexec
//                  pacman -Syu to install.
//   AurUpdates     AUR packages through paru or yay: -Qua to look; updating
//                  builds them, which wants a terminal (and a look at what's
//                  built), so it opens one.
//   AtriumRelease  atrium itself, from its GitHub releases: the package is
//                  downloaded and installed with pkexec pacman -U, and the
//                  running desktop keeps going until the next login.
//
// The first two answer to the same properties as Updates, so one page draws
// any of them. Only the desktop shell checks by itself (at login, then every
// six hours).

#include <QDateTime>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariant>

#include <memory>

class QNetworkAccessManager;

namespace atrium {

class CliUpdates : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString tool READ tool CONSTANT)          // "pacman", "paru", "yay"
    Q_PROPERTY(bool checking READ checking NOTIFY changed)
    Q_PROPERTY(bool known READ known NOTIFY changed)
    Q_PROPERTY(bool installing READ installing NOTIFY changed)
    Q_PROPERTY(QVariantList packages READ packages NOTIFY changed)  // [{ name, version, from }]
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString doing READ doing NOTIFY progressChanged)
    Q_PROPERTY(QString lastChecked READ lastChecked NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool restartNeeded READ restartNeeded NOTIFY changed)
    // Updating opens a terminal (the AUR): the button says so.
    Q_PROPERTY(bool inTerminal READ inTerminal CONSTANT)

public:
    enum class Kind { Pacman, Aur };
    explicit CliUpdates(Kind kind, QObject* parent = nullptr);

    bool available() const { return !tool_.isEmpty(); }
    QString tool() const { return tool_; }
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
    bool inTerminal() const { return kind_ == Kind::Aur; }

    Q_INVOKABLE void check();
    Q_INVOKABLE void install();
    Q_INVOKABLE void cancel();

signals:
    void changed();
    void progressChanged();

private:
    Kind kind_;
    QString tool_;
    bool checking_ = false, known_ = false, installing_ = false, restartNeeded_ = false;
    QVariantList packages_;
    int progress_ = 0;
    QString doing_, error_;
    QDateTime lastChecked_;
    std::unique_ptr<QProcess> process_;
    QByteArray output_;
    QTimer schedule_;
};

class AtriumRelease : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString current READ current CONSTANT)   // "0.1.0", as the running desktop is
    Q_PROPERTY(QString latest READ latest NOTIFY changed)   // "0.2.0", "" not known
    Q_PROPERTY(QString notes READ notes NOTIFY changed)     // the release's notes (Markdown)
    Q_PROPERTY(QString page READ page NOTIFY changed)       // its page on GitHub
    Q_PROPERTY(bool newer READ newer NOTIFY changed)        // there's one to install
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(bool checking READ checking NOTIFY changed)
    Q_PROPERTY(bool installing READ installing NOTIFY changed)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString doing READ doing NOTIFY progressChanged)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString lastChecked READ lastChecked NOTIFY changed)
    // Installed but not running yet: it takes over at the next login.
    Q_PROPERTY(bool staged READ staged NOTIFY changed)
    Q_PROPERTY(QString stagedVersion READ stagedVersion NOTIFY changed)

public:
    static AtriumRelease* instance();

    QString current() const;
    QString latest() const { return latest_; }
    QString notes() const { return notes_; }
    QString page() const { return page_; }
    bool newer() const;
    int count() const { return newer() && !staged_ ? 1 : 0; }
    bool checking() const { return checking_; }
    bool installing() const { return installing_; }
    int progress() const { return progress_; }
    QString doing() const { return doing_; }
    QString error() const { return error_; }
    QString lastChecked() const;
    bool staged() const { return staged_; }
    QString stagedVersion() const { return stagedVersion_; }

    Q_INVOKABLE void check();
    // Download the release's package and install it; staged when done.
    Q_INVOKABLE void install();

signals:
    void changed();
    void progressChanged();

private:
    AtriumRelease();
    void readInstalled();
    void fail(const QString& why);

    QNetworkAccessManager* net_ = nullptr;
    QString latest_, tag_, notes_, page_, asset_, assetName_, digest_;
    bool checking_ = false, installing_ = false, staged_ = false;
    int progress_ = 0;
    QString doing_, error_, stagedVersion_;
    QDateTime lastChecked_;
    std::unique_ptr<QProcess> pacman_;
    QTimer schedule_;
};

// "Today at 14:05", in the menu bar clock's style; "" for never.
QString checkedText(const QDateTime& when);

} // namespace atrium
