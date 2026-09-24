#pragma once
// The session's polkit agent: when an app asks for administrator rights
// (pkexec, a settings change), `PolkitAgent.flow` asks for the password.
// A password that worked in the last five minutes answers by itself
// (security.remember_admin), unseen. If another agent already serves the
// session, this one steps aside.

#include "admin_cache.hpp"

#include <PolkitQt1/Agent/Listener>
#include <PolkitQt1/Agent/Session>
#include <PolkitQt1/Identity>

#include <QObject>
#include <QPointer>

namespace atrium {

class PolkitIdentity : public QObject {
    Q_OBJECT
    Q_PROPERTY(uint id READ id CONSTANT)                    // the uid
    Q_PROPERTY(QString displayName READ displayName CONSTANT)  // "Full Name", else the user name
    Q_PROPERTY(QString string READ string CONSTANT)            // "unix-user:name"

public:
    PolkitIdentity(const PolkitQt1::Identity& identity, QObject* parent);

    uint id() const { return uid_; }
    QString displayName() const { return name_; }
    QString string() const { return identity_.toString(); }
    const PolkitQt1::Identity& identity() const { return identity_; }

private:
    PolkitQt1::Identity identity_;
    uint uid_ = 0;
    QString name_;
};

class PolkitAgent;

// One request, from asking to the answer.
class AuthFlow : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString message READ message CONSTANT)
    Q_PROPERTY(QString iconName READ iconName CONSTANT)
    Q_PROPERTY(QString actionId READ actionId CONSTANT)
    Q_PROPERTY(QList<QObject*> identities READ identities CONSTANT)
    Q_PROPERTY(QObject* selectedIdentity READ selectedIdentity WRITE setSelectedIdentity NOTIFY selectedIdentityChanged)
    Q_PROPERTY(bool isResponseRequired READ isResponseRequired NOTIFY stateChanged)
    Q_PROPERTY(bool responseVisible READ responseVisible NOTIFY stateChanged)
    Q_PROPERTY(QString inputPrompt READ inputPrompt NOTIFY stateChanged)
    Q_PROPERTY(QString supplementaryMessage READ supplementaryMessage NOTIFY stateChanged)
    Q_PROPERTY(bool supplementaryIsError READ supplementaryIsError NOTIFY stateChanged)
    Q_PROPERTY(bool isCompleted READ isCompleted NOTIFY stateChanged)

public:
    AuthFlow(PolkitAgent* agent, QString actionId, QString message, QString iconName, QString cookie,
             const PolkitQt1::Identity::List& identities, PolkitQt1::Agent::AsyncResult* result);
    ~AuthFlow() override;

    QString message() const { return message_; }
    QString iconName() const { return icon_; }
    QString actionId() const { return action_; }
    QList<QObject*> identities() const { return identities_; }
    QObject* selectedIdentity() const { return selected_; }
    void setSelectedIdentity(QObject* identity);
    bool isResponseRequired() const { return required_; }
    bool responseVisible() const { return echo_; }
    QString inputPrompt() const { return prompt_; }
    QString supplementaryMessage() const { return supplementary_; }
    bool supplementaryIsError() const { return supplementaryError_; }
    bool isCompleted() const { return completed_; }
    // Answering with the remembered password, so nothing is shown.
    bool answering() const { return answering_; }

    Q_INVOKABLE void submit(const QString& password);
    // Another admin answers instead (the next in the list).
    Q_INVOKABLE void selectNextIdentity();
    Q_INVOKABLE void cancelAuthenticationRequest();
    // polkit gave up on it (the app went away).
    void cancelled();
    // Opens a session with polkit for the selected identity.
    void start();

signals:
    void selectedIdentityChanged();
    void stateChanged();
    void authenticationSucceeded();
    void authenticationFailed();
    void authenticationRequestCancelled();

private:
    void finish(bool success, const QString& error = {});
    PolkitIdentity* identity() const { return static_cast<PolkitIdentity*>(selected_.data()); }

    PolkitAgent* agent_;
    QString action_, message_, icon_, cookie_;
    QList<QObject*> identities_;
    QPointer<QObject> selected_;
    PolkitQt1::Agent::AsyncResult* result_;
    QPointer<PolkitQt1::Agent::Session> session_;
    bool required_ = false, echo_ = false, completed_ = false, answering_ = false;
    QString prompt_, supplementary_;
    bool supplementaryError_ = false;
};

class PolkitAgent : public PolkitQt1::Agent::Listener {
    Q_OBJECT
    Q_PROPERTY(bool isActive READ isActive NOTIFY activeChanged)
    Q_PROPERTY(atrium::AuthFlow* flow READ flow NOTIFY flowChanged)
    // True while a request is open and needs the user (not answered from
    // the remembered password).
    Q_PROPERTY(bool asking READ asking NOTIFY askingChanged)

public:
    static PolkitAgent* instance();

    bool isActive() const { return active_; }
    AuthFlow* flow() const { return flow_; }
    bool asking() const;
    AdminCache& cache() { return cache_; }
    bool remember() const;

    void initiateAuthentication(const QString& actionId, const QString& message, const QString& iconName,
                                const PolkitQt1::Details& details, const QString& cookie,
                                const PolkitQt1::Identity::List& identities,
                                PolkitQt1::Agent::AsyncResult* result) override;
    bool initiateAuthenticationFinish() override;
    void cancelAuthentication() override;

    void done(AuthFlow* flow);

signals:
    void activeChanged();
    void flowChanged();
    void askingChanged();

private:
    PolkitAgent();

    bool active_ = false;
    QPointer<AuthFlow> flow_;
    AdminCache cache_;
};

} // namespace atrium
