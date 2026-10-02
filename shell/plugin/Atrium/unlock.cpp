#include "unlock.hpp"

#include <QCoreApplication>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QVariant>

#include <security/pam_appl.h>

#include <cstdlib>
#include <cstring>
#include <pwd.h>
#include <unistd.h>

namespace atrium {

namespace {

QObject* session_lock() {
    return qApp->property("atriumSessionLock").value<QObject*>();
}

int converse(int n, const pam_message** msgs, pam_response** out, void* data) {
    const auto* password = static_cast<const QByteArray*>(data);
    auto* resp = static_cast<pam_response*>(calloc(size_t(n), sizeof(pam_response)));
    if (!resp)
        return PAM_BUF_ERR;
    for (int i = 0; i < n; ++i)
        if (msgs[i]->msg_style == PAM_PROMPT_ECHO_OFF || msgs[i]->msg_style == PAM_PROMPT_ECHO_ON)
            resp[i].resp = strdup(password->constData());
    *out = resp;
    return PAM_SUCCESS;
}

} // namespace

Unlock::Unlock(QObject* parent) : QObject(parent) {
    if (QObject* lock = session_lock())
        connect(lock, SIGNAL(lockedChanged()), this, SIGNAL(changed()));
}

bool Unlock::locked() const {
    QObject* lock = session_lock();
    return lock && lock->property("locked").toBool();
}

void Unlock::clearMessage() {
    if (message_.isEmpty())
        return;
    message_.clear();
    emit changed();
}

bool Unlock::check(const QString& service, const QString& user, const QString& password, QString* error) {
    QByteArray pw = password.toUtf8();
    pam_conv conv{converse, &pw};
    pam_handle_t* pamh = nullptr;
    int r = pam_start(service.toUtf8().constData(), user.toUtf8().constData(), &conv, &pamh);
    if (r == PAM_SUCCESS)
        r = pam_authenticate(pamh, 0);
    // An expired password still unlocks (changing it is for the login).
    if (r == PAM_SUCCESS) {
        const int acct = pam_acct_mgmt(pamh, 0);
        if (acct != PAM_SUCCESS && acct != PAM_NEW_AUTHTOK_REQD)
            r = acct;
    }
    if (r == PAM_SUCCESS)
        pam_setcred(pamh, PAM_REFRESH_CRED);
    if (error && r != PAM_SUCCESS)
        *error = QString::fromLocal8Bit(pamh ? pam_strerror(pamh, r) : "PAM failed");
    if (pamh)
        pam_end(pamh, r);
    explicit_bzero(pw.data(), size_t(pw.size()));
    return r == PAM_SUCCESS;
}

void Unlock::tryPassword(const QString& password) {
    if (busy_)
        return;
    busy_ = true;
    message_.clear();
    emit changed();
    const passwd* pw = getpwuid(getuid());
    const QString user = pw ? QString::fromLocal8Bit(pw->pw_name) : QString();
    QPointer<Unlock> self(this);
    QThread* t = QThread::create([self, user, password] {
        QString error;
        const bool ok = check(QStringLiteral("atrium-lock"), user, password, &error);
        QMetaObject::invokeMethod(qApp, [self, ok, error] {
            if (self)
                self->finish(ok, error);
        });
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

void Unlock::finish(bool ok, const QString& error) {
    busy_ = false;
    if (ok) {
        if (QObject* lock = session_lock())
            QMetaObject::invokeMethod(lock, "unlock");
        emit changed();
        // Unlocked: nothing left to show.
        QTimer::singleShot(0, qApp, [] { QCoreApplication::exit(0); });
        return;
    }
    message_ = QStringLiteral("Incorrect password.");
    Q_UNUSED(error);
    emit changed();
    emit failed();
}

} // namespace atrium
