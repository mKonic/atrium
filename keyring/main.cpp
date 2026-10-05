// atrium-keyring: atrium's Secret Service (see service.hpp). atrium starts
// it at login, before anything asks for a secret. The login password comes
// from pam_atrium_keyring through a socket in the runtime dir; without it,
// the keyring asks (atrium-shell's keyring.qml) when an app first needs it.
//
// ATRIUM_SHELL and ATRIUM_SHELL_DIR say where the shell is (atrium sets them);
// ATRIUM_KEYRING_DIR overrides ~/.local/share/atrium/keyrings (tests).

#include "service.hpp"

#include <QCoreApplication>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QFile>
#include <QSocketNotifier>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>

#include <security/pam_appl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <pwd.h>

using namespace atrium::keyring;

namespace {

// What pam_atrium_keyring holds for us, once; nothing if it isn't there.
std::optional<std::string> login_password() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime)
        return std::nullopt;
    const std::string path = std::string(runtime) + "/atrium-keyring.login";
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path)
        return std::nullopt;
    std::strcpy(addr.sun_path, path.c_str());
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        if (fd >= 0)
            close(fd);
        return std::nullopt;
    }
    timeval two{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &two, sizeof two);
    std::string pw;
    char buf[256];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        pw.append(buf, size_t(n));
    }
    explicit_bzero(buf, sizeof buf);
    close(fd);
    // Only what PAM wrote, with nothing after it (the length first).
    if (pw.size() < 4)
        return std::nullopt;
    uint32_t len = 0;
    std::memcpy(&len, pw.data(), 4);
    if (len != pw.size() - 4) {
        explicit_bzero(pw.data(), pw.size());
        return std::nullopt;
    }
    std::string out = pw.substr(4);
    explicit_bzero(pw.data(), pw.size());
    return out;
}

// Whether `password` is this user's login password (PAM, as the lock
// screen checks it: through unix_chkpwd).
bool is_login_password(const std::string& password) {
    const passwd* pw = getpwuid(getuid());
    if (!pw)
        return false;
    struct Conv {
        const std::string* password;
    } data{&password};
    pam_conv conv{[](int n, const pam_message** msgs, pam_response** resp, void* d) -> int {
                      auto* replies = static_cast<pam_response*>(calloc(size_t(n), sizeof(pam_response)));
                      for (int i = 0; i < n; ++i)
                          if (msgs[i]->msg_style == PAM_PROMPT_ECHO_OFF)
                              replies[i].resp = strdup(static_cast<Conv*>(d)->password->c_str());
                      *resp = replies;
                      return PAM_SUCCESS;
                  },
                  &data};
    pam_handle_t* h = nullptr;
    if (pam_start("atrium-lock", pw->pw_name, &conv, &h) != PAM_SUCCESS)
        return false;
    const int r = pam_authenticate(h, 0);
    pam_end(h, r);
    return r == PAM_SUCCESS;
}

QString shell_program() {
    const QString env = qEnvironmentVariable("ATRIUM_SHELL");
    return env.isEmpty() ? QStandardPaths::findExecutable("atrium-shell") : env;
}

QString shell_dir() {
    const QString env = qEnvironmentVariable("ATRIUM_SHELL_DIR");
    return env.isEmpty() ? QStringLiteral("/usr/share/atrium/shell") : env;
}

// The dialog: the shell's keyring.qml, the password on its stdout, exit 1
// for Cancel. Making the keyring takes the login password: another is
// asked again.
void dialog(const QString& mode, const QString& who, bool wrong, std::function<void(std::optional<std::string>)> answer) {
    auto* p = new QProcess(qApp);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("ATRIUM_KEYRING_MODE", mode);
    env.insert("ATRIUM_KEYRING_WHO", who);
    env.insert("ATRIUM_KEYRING_WRONG", wrong ? "1" : "");
    p->setProcessEnvironment(env);
    QObject::connect(p, &QProcess::finished, p, [p, mode, who, answer](int code, QProcess::ExitStatus st) {
        p->deleteLater();
        QByteArray out = p->readAllStandardOutput();
        if (st != QProcess::NormalExit || code != 0) {
            answer(std::nullopt);
            return;
        }
        if (out.endsWith('\n'))
            out.chop(1);
        std::string pw = out.toStdString();
        explicit_bzero(out.data(), size_t(out.size()));
        if (mode == "create" && !is_login_password(pw)) {
            explicit_bzero(pw.data(), pw.size());
            dialog(mode, who, true, answer);
            return;
        }
        answer(std::move(pw));
    });
    QObject::connect(p, &QProcess::errorOccurred, p, [p, answer](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            p->deleteLater();
            answer(std::nullopt);
        }
    });
    p->start(shell_program(), {shell_dir() + "/keyring.qml"});
}

// Whether the one serving now is another atrium-keyring (a crashed atrium's
// restart starting it again): that one stays, open as it was.
bool already_serving() {
    QDBusConnectionInterface* dbus = QDBusConnection::sessionBus().interface();
    if (!dbus)
        return false;
    const QDBusReply<uint> pid = dbus->servicePid("org.freedesktop.secrets");
    if (!pid.isValid())
        return false;
    QFile comm(QString("/proc/%1/comm").arg(pid.value()));
    return comm.open(QIODevice::ReadOnly) && comm.readAll().trimmed() == "atrium-keyring" &&
           pid_t(pid.value()) != getpid();
}

} // namespace

int main(int argc, char** argv) {
    // Started by atrium: it goes with the session, and the keyring's open
    // contents with it (not left unlocked after logging out). The session
    // is atrium's crash-recovery wrapper when there is one
    // (ATRIUM_KEYRING_OUTLIVE), else atrium itself.
    int session_fd = -1;
    if (const QByteArray outlive = qgetenv("ATRIUM_KEYRING_OUTLIVE"); !outlive.isEmpty()) {
        session_fd = int(syscall(SYS_pidfd_open, pid_t(outlive.toInt()), 0));
        if (session_fd < 0)
            return 0;  // already gone
    } else if (qEnvironmentVariableIsSet("ATRIUM_SHELL")) {
        const pid_t parent = getppid();
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent)
            return 0;
    }
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("atrium-keyring");
    if (already_serving()) {
        qInfo("atrium-keyring: already running");
        return 0;
    }
    if (session_fd >= 0) {
        auto* gone = new QSocketNotifier(session_fd, QSocketNotifier::Read, &app);
        // As the parent-death signal would: whatever it's in the middle of
        // (a first prompt's loop too).
        QObject::connect(gone, &QSocketNotifier::activated, &app, [] { ::kill(getpid(), SIGTERM); });
    }

    QString dir = qEnvironmentVariable("ATRIUM_KEYRING_DIR");
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/atrium/keyrings";

    auto password = login_password();
    Service service(dir, password);
    if (password)
        explicit_bzero(password->data(), password->size());

    service.set_ask(dialog);

    if (!service.start()) {
        qCritical("atrium-keyring: can't serve on the session bus");
        return 1;
    }
    return app.exec();
}
