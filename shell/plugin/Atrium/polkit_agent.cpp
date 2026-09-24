#include "polkit_agent.hpp"

#include "compositor.hpp"

#include <PolkitQt1/Subject>

#include <QCoreApplication>

#include <pwd.h>
#include <unistd.h>

#include <deque>

namespace atrium {

namespace {

// Requests that came while one was open, answered in turn.
std::deque<QPointer<AuthFlow>>& queue() {
    static std::deque<QPointer<AuthFlow>> q;
    return q;
}

} // namespace

// --- PolkitIdentity ------------------------------------------------------------

PolkitIdentity::PolkitIdentity(const PolkitQt1::Identity& identity, QObject* parent)
    : QObject(parent), identity_(identity) {
    PolkitQt1::Identity copy = identity;
    const PolkitQt1::UnixUserIdentity user = copy.toUnixUserIdentity();
    if (user.isValid()) {
        uid_ = user.uid();
        if (const passwd* pw = getpwuid(uid_)) {
            // The GECOS full name (up to the first comma), else the login.
            const QString gecos = QString::fromLocal8Bit(pw->pw_gecos).section(',', 0, 0);
            name_ = gecos.isEmpty() ? QString::fromLocal8Bit(pw->pw_name) : gecos;
        }
    }
    if (name_.isEmpty())
        name_ = identity.toString().section(':', 1);
}

// --- AuthFlow ------------------------------------------------------------------

AuthFlow::AuthFlow(PolkitAgent* agent, QString actionId, QString message, QString iconName, QString cookie,
                   const PolkitQt1::Identity::List& identities, PolkitQt1::Agent::AsyncResult* result)
    : QObject(agent), agent_(agent), action_(std::move(actionId)), message_(std::move(message)),
      icon_(std::move(iconName)), cookie_(std::move(cookie)), result_(result) {
    for (const PolkitQt1::Identity& i : identities)
        identities_.append(new PolkitIdentity(i, this));
    // The user at the keyboard, if they may answer; else the first admin.
    for (QObject* o : identities_)
        if (static_cast<PolkitIdentity*>(o)->id() == getuid())
            selected_ = o;
    if (!selected_ && !identities_.isEmpty())
        selected_ = identities_.first();
}

AuthFlow::~AuthFlow() {
    if (session_)
        session_->deleteLater();
    if (!completed_ && result_) {
        result_->setError(QStringLiteral("the agent went away"));
        result_->setCompleted();
    }
    delete result_;
}

void AuthFlow::start() {
    if (session_) {
        session_->cancel();
        session_->deleteLater();
    }
    required_ = false;
    prompt_.clear();
    if (!identity()) {
        finish(false, QStringLiteral("no identity to authenticate as"));
        return;
    }
    auto* s = new PolkitQt1::Agent::Session(identity()->identity(), cookie_, nullptr, this);
    session_ = s;
    connect(s, &PolkitQt1::Agent::Session::request, this, [this, s](const QString& request, bool echo) {
        if (s != session_)
            return;
        prompt_ = request;
        echo_ = echo;
        // A password that worked moments ago answers without asking.
        if (!echo && !answering_ && agent_->remember()) {
            const QString pw = agent_->cache().recall(identity()->id());
            if (!pw.isEmpty()) {
                answering_ = true;
                agent_->cache().stage(identity()->id(), pw);
                s->setResponse(pw);
                emit stateChanged();
                return;
            }
        }
        answering_ = false;
        required_ = true;
        emit stateChanged();
        emit agent_->askingChanged();
    });
    connect(s, &PolkitQt1::Agent::Session::showError, this, [this](const QString& text) {
        supplementary_ = text;
        supplementaryError_ = true;
        emit stateChanged();
    });
    connect(s, &PolkitQt1::Agent::Session::showInfo, this, [this](const QString& text) {
        supplementary_ = text;
        supplementaryError_ = false;
        emit stateChanged();
    });
    connect(s, &PolkitQt1::Agent::Session::completed, this, [this, s](bool gained) {
        if (s != session_)
            return;
        if (gained) {
            if (agent_->remember())
                agent_->cache().commit();
            emit authenticationSucceeded();
            finish(true);
            return;
        }
        if (answering_) {
            // The remembered password no longer works: forget it and ask.
            agent_->cache().forget();
        } else {
            emit authenticationFailed();
        }
        start();  // another try, as polkit allows
        // Remembered or not, the next answer comes from the user.
        answering_ = false;
    });
    s->initiate();
}

void AuthFlow::setSelectedIdentity(QObject* identity) {
    if (identity == selected_ || !identities_.contains(identity))
        return;
    selected_ = identity;
    emit selectedIdentityChanged();
    if (!completed_)
        start();
}

void AuthFlow::submit(const QString& password) {
    if (!required_ || !session_ || password.isEmpty())
        return;
    agent_->cache().stage(identity()->id(), password);
    required_ = false;
    supplementary_.clear();
    emit stateChanged();
    session_->setResponse(password);
}

void AuthFlow::cancelAuthenticationRequest() {
    if (completed_)
        return;
    if (session_)
        session_->cancel();
    emit authenticationRequestCancelled();
    finish(false, QStringLiteral("cancelled by the user"));
}

void AuthFlow::cancelled() {
    if (completed_)
        return;
    if (session_)
        session_->cancel();
    emit authenticationRequestCancelled();
    finish(false, QStringLiteral("cancelled"));
}

void AuthFlow::finish(bool success, const QString& error) {
    if (completed_)
        return;
    completed_ = true;
    required_ = false;
    answering_ = false;
    if (!success)
        result_->setError(error);
    result_->setCompleted();
    emit stateChanged();
    agent_->done(this);
}

// --- PolkitAgent ---------------------------------------------------------------

PolkitAgent* PolkitAgent::instance() {
    static auto* self = new PolkitAgent;
    return self;
}

PolkitAgent::PolkitAgent() {
    const PolkitQt1::UnixSessionSubject subject(QCoreApplication::applicationPid());
    active_ = registerListener(subject, QStringLiteral("/org/atrium/PolicyKit1/AuthenticationAgent"));
    if (!active_)
        qWarning("polkit: another agent serves this session; stepping aside");
    // Turning the setting off forgets at once.
    connect(Compositor::instance(), &Compositor::settingsChanged, this, [this] {
        if (!remember())
            cache_.forget();
    });
}

bool PolkitAgent::remember() const {
    return Compositor::instance()->settings().value("security.remember_admin", true).toBool();
}

bool PolkitAgent::asking() const {
    return flow_ && !flow_->isCompleted() && !flow_->answering();
}

void PolkitAgent::initiateAuthentication(const QString& actionId, const QString& message, const QString& iconName,
                                         const PolkitQt1::Details&, const QString& cookie,
                                         const PolkitQt1::Identity::List& identities,
                                         PolkitQt1::Agent::AsyncResult* result) {
    auto* f = new AuthFlow(this, actionId, message, iconName, cookie, identities, result);
    connect(f, &AuthFlow::stateChanged, this, &PolkitAgent::askingChanged);
    if (flow_) {
        queue().push_back(f);
        return;
    }
    flow_ = f;
    emit flowChanged();
    f->start();
}

bool PolkitAgent::initiateAuthenticationFinish() {
    return true;
}

void PolkitAgent::cancelAuthentication() {
    if (flow_)
        flow_->cancelled();
}

void PolkitAgent::done(AuthFlow* flow) {
    if (flow != flow_)
        return;
    flow->deleteLater();
    flow_ = nullptr;
    while (!queue().empty() && !flow_) {
        flow_ = queue().front();
        queue().pop_front();
    }
    emit flowChanged();
    emit askingChanged();
    if (flow_)
        flow_->start();
}

} // namespace atrium
