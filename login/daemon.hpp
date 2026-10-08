#pragma once
// atrium-login's daemon: shows the greeter, logs people in through a PAM
// worker per session, and brings the greeter back when a session ends. The
// greeter talks greetd's protocol on GREETD_SOCK.
//
// Sessions each keep a VT of their own. A session asks for a greeter on
// the control socket (Switch User): it comes up on a free VT while the
// others run on, and logging in as someone who already has a session
// switches to that session instead of starting another.
#include "core.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <pwd.h>
#include <string>
#include <sys/types.h>
#include <vector>

namespace atrium::login {

class Daemon {
public:
    explicit Daemon(Config config) : config_(std::move(config)) {}
    ~Daemon();
    // Until SIGTERM; the exit status.
    int run();

private:
    struct Worker {
        pid_t pid = -1;
        int fd = -1;
        int vt = 0;
        int tty = -1;  // held open: the VT is ours (logind spawns no getty on it)
        std::string user;
        uid_t uid = 0;
        std::string session_id;  // logind's, once started
        bool authenticated = false;
        bool asking = false;  // waits for the greeter's answer
        bool running = false;
    };
    using WorkerPtr = std::unique_ptr<Worker>;
    using Clock = std::chrono::steady_clock;

    WorkerPtr spawn(const std::string& service, const std::string& user, const std::string& cls, bool conversation,
                    int vt);
    void start(Worker& w, const std::vector<std::string>& cmd, const std::vector<std::string>& env, bool profile);
    void end(WorkerPtr& w);
    void start_greeter(int vt = 0);
    bool open_greeter_socket(uid_t uid, gid_t gid);
    bool open_control_socket();
    void own_home(const passwd& pw);
    void activate_vt(int vt);
    int active_vt();
    int free_vt();
    void switch_to(Worker& session);
    Worker* session_of(const std::string& user);

    void on_worker(WorkerPtr& w);
    void on_worker_gone(WorkerPtr& w);
    void on_session_gone(size_t index);
    void on_client();
    void handle(const Request& r);
    void reply(const std::string& body);
    void drop_client();
    void start_pending();
    void back_from_greeter();
    void on_control(size_t index);
    void switch_to_greeter(uid_t asker);

    Config config_;
    WorkerPtr greeter_, pending_;
    std::vector<WorkerPtr> sessions_;
    int signal_fd_ = -1, listen_fd_ = -1, client_fd_ = -1, control_fd_ = -1;
    std::string socket_path_, client_buffer_;
    struct ControlClient {
        int fd;
        std::string buffer;
    };
    std::vector<ControlClient> controls_;
    // A started session waits for the greeter to go.
    std::optional<std::pair<std::vector<std::string>, std::vector<std::string>>> to_start_;
    std::optional<Clock::time_point> greeter_deadline_, greeter_restart_;
    std::vector<Clock::time_point> greeter_starts_;
    // The VT a greeter brought up beside running sessions goes back to when
    // it closes without a login (0: none, it's the only thing running).
    int return_vt_ = 0;
    bool quitting_ = false;
};

} // namespace atrium::login
