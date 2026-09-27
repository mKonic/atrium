#include "update_sources.hpp"

#include "compositor.hpp"
#include "desktop_entries.hpp"
#include "updates_core.hpp"
#include "version.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <QStandardPaths>

namespace atrium {

namespace {

constexpr int kFirstCheckMs = 90 * 1000;
constexpr int kEveryMs = 6 * 60 * 60 * 1000;
const char* const kLatest = "https://api.github.com/repos/mKonic/atrium/releases/latest";

bool isDesktopShell() {
    return QGuiApplication::desktopFileName() == "atrium-shell";
}

QString cacheDir() {
    QString dir = qEnvironmentVariable("XDG_CACHE_HOME");
    if (dir.isEmpty())
        dir = QDir::homePath() + "/.cache";
    return dir + "/atrium/updates";
}

} // namespace

QString checkedText(const QDateTime& when) {
    if (!when.isValid())
        return {};
    const QDate day = when.date(), today = QDate::currentDate();
    const bool h24 = Compositor::instance()->settings().value("clock.24_hour").toBool();
    const QString time = QLocale().toString(when.time(), h24 ? QStringLiteral("HH:mm") : QStringLiteral("h:mm AP"));
    if (day == today)
        return "Today at " + time;
    if (day == today.addDays(-1))
        return "Yesterday at " + time;
    return QLocale().toString(day, QLocale::ShortFormat) + " at " + time;
}

// --- pacman and the AUR ---------------------------------------------------------------

CliUpdates::CliUpdates(Kind kind, QObject* parent) : QObject(parent), kind_(kind) {
    if (kind == Kind::Pacman) {
        if (!QStandardPaths::findExecutable("checkupdates").isEmpty() && !QStandardPaths::findExecutable("pacman").isEmpty())
            tool_ = "pacman";
    } else {
        for (const char* helper : {"paru", "yay"})
            if (!QStandardPaths::findExecutable(helper).isEmpty()) {
                tool_ = helper;
                break;
            }
    }
    if (available() && isDesktopShell()) {
        connect(&schedule_, &QTimer::timeout, this, [this] {
            schedule_.setInterval(kEveryMs);
            if (!installing_ && !checking_)
                check();
        });
        schedule_.start(kFirstCheckMs);
    }
}

QString CliUpdates::summary() const {
    return QString::fromStdString(updates::summary(count(), 0));
}

QString CliUpdates::lastChecked() const {
    return checkedText(lastChecked_);
}

void CliUpdates::check() {
    if (!available() || checking_ || installing_)
        return;
    checking_ = true;
    error_.clear();
    emit changed();
    process_ = std::make_unique<QProcess>();
    QProcess* p = process_.get();
    output_.clear();
    connect(p, &QProcess::readyReadStandardOutput, this, [this, p] { output_ += p->readAllStandardOutput(); });
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
        output_ += p->readAllStandardOutput();
        const QString err = QString::fromUtf8(p->readAllStandardError()).trimmed();
        checking_ = false;
        lastChecked_ = QDateTime::currentDateTime();
        // checkupdates: 0 some, 2 none, 1 failed; -Qua: 0 some, 1 none.
        const bool none = (kind_ == Kind::Pacman && code == 2) || (kind_ == Kind::Aur && code == 1 && output_.isEmpty());
        if (code != 0 && !none) {
            error_ = err.isEmpty() ? QString("%1 couldn't check (exit %2).").arg(tool_).arg(code) : err.section('\n', -1);
        } else {
            QVariantList list;
            for (const updates::Upgrade& u : updates::parse_upgrades(output_.toStdString()))
                list.push_back(QVariantMap{{"name", QString::fromStdString(u.name)},
                                           {"version", QString::fromStdString(u.to)},
                                           {"from", QString::fromStdString(u.from)},
                                           {"summary", QString("%1 → %2").arg(QString::fromStdString(u.from), QString::fromStdString(u.to))}});
            packages_ = list;
            known_ = true;
        }
        emit changed();
        process_.release()->deleteLater();
    });
    if (kind_ == Kind::Pacman)
        p->start("checkupdates", {"--nocolor"});
    else
        p->start(tool_, {"-Qua"});
}

void CliUpdates::install() {
    if (!available() || installing_ || checking_)
        return;
    installing_ = true;
    progress_ = 0;
    doing_.clear();
    error_.clear();
    emit changed();
    emit progressChanged();
    process_ = std::make_unique<QProcess>();
    QProcess* p = process_.get();
    output_.clear();
    const QVariantList updating = packages_;
    if (kind_ == Kind::Pacman) {
        // Everything at once, as pacman wants it (partial upgrades break things).
        p->setProcessChannelMode(QProcess::MergedChannels);
        connect(p, &QProcess::readyRead, this, [this, p] {
            output_ += p->readAll();
            qsizetype nl;
            while ((nl = output_.indexOf('\n')) >= 0) {
                const QByteArray line = output_.left(nl);
                output_.remove(0, nl + 1);
                updates::Step s;
                if (updates::parse_pacman_step(line.toStdString(), s)) {
                    progress_ = s.done * 100 / s.total;
                    doing_ = QString::fromStdString(s.what);
                    emit progressChanged();
                } else if (line.startsWith("error:")) {
                    error_ = QString::fromUtf8(line.mid(6)).trimmed();
                }
            }
        });
        p->setProgram("pkexec");
        p->setArguments({"pacman", "-Syu", "--noconfirm"});
    } else {
        // AUR packages are built from their PKGBUILDs: that's for a terminal,
        // where they can be looked at before anything runs.
        QStringList argv = shell::DesktopEntries::inTerminal({tool_, "-Sua"});
        if (argv.isEmpty()) {
            installing_ = false;
            error_ = "No terminal to update in.";
            emit changed();
            return;
        }
        p->setProgram(argv.takeFirst());
        p->setArguments(argv);
    }
    connect(p, &QProcess::finished, this, [this, updating](int code, QProcess::ExitStatus) {
        installing_ = false;
        doing_.clear();
        if (kind_ == Kind::Pacman) {
            if (code == 0) {
                progress_ = 100;
                for (const QVariant& v : updating)
                    if (updates::needs_restart(v.toMap().value("name").toString().toStdString()))
                        restartNeeded_ = true;
            } else if (error_.isEmpty()) {
                // 126/127: pkexec's own (cancelled, not allowed).
                error_ = code == 126 || code == 127 ? QStringLiteral("Not allowed.") : QString("pacman stopped (exit %1).").arg(code);
            }
        }
        emit progressChanged();
        emit changed();
        process_.release()->deleteLater();
        check();  // what's still newer
    });
    p->start();
}

void CliUpdates::cancel() {
    // pacman mid-transaction is left to finish: stopping it half way leaves a broken system.
    if (process_ && checking_)
        process_->kill();
}

// --- atrium itself ------------------------------------------------------------------------

AtriumRelease* AtriumRelease::instance() {
    static auto* self = new AtriumRelease;
    return self;
}

AtriumRelease::AtriumRelease() {
    readInstalled();
    if (isDesktopShell()) {
        connect(&schedule_, &QTimer::timeout, this, [this] {
            schedule_.setInterval(kEveryMs);
            if (!installing_ && !checking_)
                check();
        });
        schedule_.start(kFirstCheckMs);
    }
}

QString AtriumRelease::current() const {
    return QString::fromStdString(updates::pkgver(ATRIUM_VERSION, ATRIUM_BUILD));
}

bool AtriumRelease::newer() const {
    return !tag_.isEmpty() && updates::release_newer(tag_.toStdString(), ATRIUM_VERSION);
}

QString AtriumRelease::lastChecked() const {
    return checkedText(lastChecked_);
}

// What pacman has installed, against what is running: a newer one installed
// (by Software Update or by hand) runs from the next login.
void AtriumRelease::readInstalled() {
    QProcess q;
    q.start("pacman", {"-Q", "atrium-git"});
    if (!q.waitForFinished(3000) || q.exitCode() != 0)
        return;
    // "atrium-git 0.2.0-1": the version without its package release.
    QString installed = QString::fromUtf8(q.readAllStandardOutput()).trimmed().section(' ', 1);
    installed = installed.left(installed.lastIndexOf('-'));
    const bool was = staged_;
    // Newer than what runs, by pacman's own order (an older package under a
    // development build isn't waiting to take over).
    bool newer = false;
    if (!installed.isEmpty() && installed != current()) {
        QProcess cmp;
        cmp.start("vercmp", {installed, current()});
        newer = cmp.waitForFinished(2000) && QString::fromUtf8(cmp.readAllStandardOutput()).trimmed().toInt() > 0;
    }
    staged_ = newer;
    stagedVersion_ = staged_ ? installed : QString();
    if (staged_ != was)
        emit changed();
}

void AtriumRelease::fail(const QString& why) {
    checking_ = installing_ = false;
    error_ = why;
    doing_.clear();
    emit progressChanged();
    emit changed();
}

void AtriumRelease::check() {
    if (checking_ || installing_)
        return;
    if (!net_)
        net_ = new QNetworkAccessManager(this);
    checking_ = true;
    error_.clear();
    emit changed();
    QNetworkRequest req{QUrl(kLatest)};
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "atrium/" ATRIUM_VERSION);
    req.setTransferTimeout(15000);
    QNetworkReply* reply = net_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        lastChecked_ = QDateTime::currentDateTime();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 404) {  // nothing released yet
            checking_ = false;
            emit changed();
            return;
        }
        if (reply->error() != QNetworkReply::NoError)
            return fail("Couldn't reach GitHub: " + reply->errorString());
        const QJsonObject r = QJsonDocument::fromJson(reply->readAll()).object();
        tag_ = r.value("tag_name").toString();
        latest_ = QString::fromStdString(updates::pkgver(tag_.toStdString(), 0));
        notes_ = r.value("body").toString();
        page_ = r.value("html_url").toString();
        asset_.clear();
        for (const QJsonValue& a : r.value("assets").toArray()) {
            const QJsonObject o = a.toObject();
            const QString name = o.value("name").toString();
            if (name.endsWith("-x86_64.pkg.tar.zst")) {
                asset_ = o.value("browser_download_url").toString();
                assetName_ = name;
                digest_ = o.value("digest").toString();  // "sha256:…", when GitHub gives it
            }
        }
        checking_ = false;
        readInstalled();
        emit changed();
    });
}

void AtriumRelease::install() {
    if (installing_ || checking_ || !newer())
        return;
    if (asset_.isEmpty())
        return fail("This release has no package to install yet.");
    if (!net_)
        net_ = new QNetworkAccessManager(this);
    installing_ = true;
    progress_ = 0;
    error_.clear();
    doing_ = "Downloading atrium " + latest_;
    emit changed();
    emit progressChanged();
    QNetworkRequest req{QUrl(asset_)};
    req.setRawHeader("User-Agent", "atrium/" ATRIUM_VERSION);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = net_->get(req);
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        if (total > 0) {
            progress_ = int(got * 90 / total);  // the rest is installing
            emit progressChanged();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return fail("The download failed: " + reply->errorString());
        const QByteArray body = reply->readAll();
        if (digest_.startsWith("sha256:") &&
            QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex() != digest_.mid(7).toLatin1())
            return fail("The download doesn't match the release; nothing was installed.");
        QDir().mkpath(cacheDir());
        const QString file = cacheDir() + "/" + assetName_;
        QSaveFile out(file);
        if (!out.open(QIODevice::WriteOnly) || out.write(body) != body.size() || !out.commit())
            return fail("Couldn't save the download.");
        doing_ = "Installing atrium " + latest_;
        progress_ = 90;
        emit progressChanged();
        // The running desktop isn't touched: the new one starts at the next login.
        pacman_ = std::make_unique<QProcess>();
        QProcess* p = pacman_.get();
        p->setProcessChannelMode(QProcess::MergedChannels);
        connect(p, &QProcess::finished, this, [this, p, file](int code, QProcess::ExitStatus) {
            const QString said = QString::fromUtf8(p->readAll());
            pacman_.release()->deleteLater();
            if (code != 0) {
                const QString err = said.section("error:", -1).trimmed().section('\n', 0, 0);
                return fail(code == 126 || code == 127 ? QStringLiteral("Not allowed.")
                                                       : err.isEmpty() ? QString("pacman stopped (exit %1).").arg(code) : err);
            }
            QFile::remove(file);
            installing_ = false;
            progress_ = 100;
            doing_.clear();
            readInstalled();
            emit progressChanged();
            emit changed();
        });
        p->start("pkexec", {"pacman", "-U", "--noconfirm", file});
    });
}

} // namespace atrium
