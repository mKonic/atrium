#include "worker.hpp"

#include "core.hpp"

#include <security/pam_appl.h>

#ifdef ATRIUM_JOURNAL
#include <systemd/sd-journal.h>
#endif

#include <cerrno>
#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

namespace atrium::login {

using nlohmann::json;

bool send_message(int fd, const json& m) {
    const std::string s = m.dump();
    return send(fd, s.data(), s.size(), MSG_NOSIGNAL) == ssize_t(s.size());
}

std::optional<json> receive_message(int fd) {
    std::string buf(64 * 1024, '\0');
    ssize_t n;
    do
        n = recv(fd, buf.data(), buf.size(), 0);
    while (n < 0 && errno == EINTR);
    if (n <= 0)
        return std::nullopt;
    json j = json::parse(std::string_view(buf.data(), size_t(n)), nullptr, false);
    if (!j.is_object() || !j.contains("t"))
        return std::nullopt;
    return j;
}

namespace {

struct State {
    int fd;
    bool conversation;  // may ask (a user logging in), else refuse to
};

__attribute__((format(printf, 1, 2))) void say(const char* fmt, ...) {
    std::fprintf(stderr, "atrium-login worker: ");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}

int converse(int n, const pam_message** msgs, pam_response** out, void* data) {
    auto* st = static_cast<State*>(data);
    if (!st->conversation || n <= 0)
        return PAM_CONV_ERR;
    auto* resp = static_cast<pam_response*>(calloc(size_t(n), sizeof(pam_response)));
    if (!resp)
        return PAM_BUF_ERR;
    auto fail = [&] {
        for (int i = 0; i < n; ++i)
            if (resp[i].resp) {
                explicit_bzero(resp[i].resp, strlen(resp[i].resp));
                free(resp[i].resp);
            }
        free(resp);
        return PAM_CONV_ERR;
    };
    for (int i = 0; i < n; ++i) {
        const char* kind = "info";
        switch (msgs[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF: kind = "secret"; break;
        case PAM_PROMPT_ECHO_ON: kind = "visible"; break;
        case PAM_ERROR_MSG: kind = "error"; break;
        default: break;
        }
        if (!send_message(st->fd, {{"t", "ask"}, {"kind", kind}, {"text", msgs[i]->msg ? msgs[i]->msg : ""}}))
            return fail();
        std::optional<json> m = receive_message(st->fd);
        if (!m || (*m)["t"] != "answer")
            return fail();  // cancelled, or the daemon is gone
        if (msgs[i]->msg_style == PAM_PROMPT_ECHO_OFF || msgs[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            std::string text = m->contains("text") && (*m)["text"].is_string() ? (*m)["text"].get<std::string>() : "";
            resp[i].resp = strdup(text.c_str());
            explicit_bzero(text.data(), text.size());
            if (!resp[i].resp)
                return fail();
        }
    }
    *out = resp;
    return PAM_SUCCESS;
}

bool wrong_credentials(int r) {
    return r == PAM_AUTH_ERR || r == PAM_USER_UNKNOWN || r == PAM_MAXTRIES || r == PAM_CRED_INSUFFICIENT ||
           r == PAM_AUTHINFO_UNAVAIL || r == PAM_PERM_DENIED;
}

// The session process: the VT as its controlling terminal, the user's ids,
// its home, and the command.
[[noreturn]] void exec_session(const passwd& pw, int vt, const std::string& cls, const std::vector<std::string>& argv,
                               const std::vector<std::string>& env) {
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    for (int sig : {SIGCHLD, SIGTERM, SIGINT, SIGHUP, SIGPIPE})
        signal(sig, SIG_DFL);
    setsid();
    const std::string tty = "/dev/tty" + std::to_string(vt);
    if (int t = open(tty.c_str(), O_RDWR | O_NOCTTY); t >= 0) {
        ioctl(t, TIOCSCTTY, 0);
        dup2(t, STDIN_FILENO);
        if (t > STDERR_FILENO)
            close(t);
    }
    int log = -1;
#ifdef ATRIUM_JOURNAL
    log = sd_journal_stream_fd(cls == "greeter" ? "atrium-greeter" : "atrium-session", LOG_INFO, 0);
#endif
    if (log < 0)
        log = open("/dev/null", O_WRONLY);
    if (log >= 0) {
        dup2(log, STDOUT_FILENO);
        dup2(log, STDERR_FILENO);
        if (log > STDERR_FILENO)
            close(log);
    }
    if (setgid(pw.pw_gid) != 0 || setuid(pw.pw_uid) != 0) {
        say("can't become %s", pw.pw_name);
        _exit(1);
    }
    if (chdir(pw.pw_dir) != 0 && chdir("/") != 0)
        _exit(1);
    std::vector<char*> args, envp;
    for (const std::string& a : argv)
        args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    for (const std::string& e : env)
        envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);
    execve(args[0], args.data(), envp.data());
    say("can't run %s: %s", args[0], strerror(errno));
    _exit(127);
}

} // namespace

void run_worker(int fd) {
    std::optional<json> init = receive_message(fd);
    if (!init || (*init)["t"] != "init")
        _exit(1);
    const std::string service = init->value("service", "");
    const std::string user = init->value("user", "");
    const std::string cls = init->value("class", "user");
    const int vt = init->value("vt", 1);
    State st{fd, init->value("auth", false)};

    pam_conv conv{converse, &st};
    pam_handle_t* pamh = nullptr;
    int r = pam_start(service.c_str(), user.c_str(), &conv, &pamh);
    auto report = [&](int code) {
        send_message(fd, {{"t", "auth"}, {"ok", false}, {"wrong", wrong_credentials(code)},
                          {"error", pamh ? pam_strerror(pamh, code) : "PAM failed"}});
        if (pamh)
            pam_end(pamh, code);
        _exit(1);
    };
    if (r != PAM_SUCCESS)
        report(r);
    const std::string tty = "tty" + std::to_string(vt);
    pam_set_item(pamh, PAM_TTY, tty.c_str());
    // What pam_systemd registers the session with.
    for (const std::string& e : {std::string("XDG_SESSION_TYPE=wayland"), "XDG_SESSION_CLASS=" + cls, std::string("XDG_SEAT=seat0"),
                                 "XDG_VTNR=" + std::to_string(vt)})
        pam_putenv(pamh, e.c_str());
    if (cls != "greeter")
        pam_putenv(pamh, "XDG_SESSION_DESKTOP=atrium");

    if ((r = pam_authenticate(pamh, 0)) != PAM_SUCCESS)
        report(r);
    r = pam_acct_mgmt(pamh, 0);
    if (r == PAM_NEW_AUTHTOK_REQD)
        r = pam_chauthtok(pamh, PAM_CHANGE_EXPIRED_AUTHTOK);
    if (r != PAM_SUCCESS)
        report(r);
    st.conversation = false;  // nothing more to ask
    send_message(fd, {{"t", "auth"}, {"ok", true}});

    std::optional<json> start = receive_message(fd);
    if (!start || (*start)["t"] != "start") {
        pam_end(pamh, PAM_SUCCESS);
        _exit(0);
    }
    passwd* pw = getpwnam(user.c_str());
    if (!pw) {
        say("no account %s", user.c_str());
        pam_end(pamh, PAM_USER_UNKNOWN);
        _exit(1);
    }
    const passwd account = *pw;
    const std::string name = account.pw_name, home = account.pw_dir, shell = account.pw_shell ? account.pw_shell : "";
    if (initgroups(name.c_str(), account.pw_gid) != 0)
        say("initgroups failed: %s", strerror(errno));
    if ((r = pam_setcred(pamh, PAM_ESTABLISH_CRED)) != PAM_SUCCESS || (r = pam_open_session(pamh, 0)) != PAM_SUCCESS) {
        say("can't open the session: %s", pam_strerror(pamh, r));
        pam_setcred(pamh, PAM_DELETE_CRED);
        pam_end(pamh, r);
        send_message(fd, {{"t", "done"}});
        _exit(1);
    }

    std::vector<std::string> pam_env;
    if (char** list = pam_getenvlist(pamh)) {
        for (char** e = list; *e; ++e) {
            pam_env.emplace_back(*e);
            free(*e);
        }
        free(list);
    }
    std::vector<std::string> cmd = start->value("cmd", std::vector<std::string>{});
    std::vector<std::string> requested = start->value("env", std::vector<std::string>{});
    const std::vector<std::string> env = session_env(pam_env, {name, home, shell}, requested);
    const std::vector<std::string> argv = session_argv(cmd, start->value("profile", true));

    const pid_t child = fork();
    if (child == 0) {
        close(fd);
        passwd copy = account;
        copy.pw_name = const_cast<char*>(name.c_str());
        copy.pw_dir = const_cast<char*>(home.c_str());
        exec_session(copy, vt, cls, argv, env);
    }
    if (child > 0) {
        send_message(fd, {{"t", "started"}, {"pid", child}});
        const int pidfd = int(syscall(SYS_pidfd_open, child, 0));
        bool stopping = false;
        int status = 0;
        for (;;) {
            pollfd p[2] = {{pidfd, POLLIN, 0}, {stopping ? -1 : fd, POLLIN, 0}};
            const int n = poll(p, 2, stopping ? 5000 : -1);
            if (n < 0 && errno == EINTR)
                continue;
            if (n == 0) {  // it didn't take the hint
                kill(-child, SIGKILL);
                kill(child, SIGKILL);
            }
            if (pidfd < 0 || (p[0].revents & POLLIN)) {
                if (waitpid(child, &status, pidfd < 0 ? 0 : WNOHANG) == child)
                    break;
            }
            if (p[1].revents) {
                // Told to stop, or the daemon went: end the session.
                std::optional<json> m = (p[1].revents & POLLIN) ? receive_message(fd) : std::nullopt;
                if (!m || (*m)["t"] == "stop") {
                    stopping = true;
                    kill(-child, SIGTERM);
                    kill(child, SIGTERM);
                }
            }
        }
        if (pidfd >= 0)
            close(pidfd);
    } else {
        say("fork failed: %s", strerror(errno));
    }
    pam_close_session(pamh, 0);
    pam_setcred(pamh, PAM_DELETE_CRED);
    pam_end(pamh, PAM_SUCCESS);
    send_message(fd, {{"t", "done"}});
    _exit(0);
}

} // namespace atrium::login
