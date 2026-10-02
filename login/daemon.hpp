#pragma once
// atrium-login's daemon: shows the greeter on its VT, logs people in
// through a PAM worker per session, and brings the greeter back when a
// session ends. The greeter talks greetd's protocol on GREETD_SOCK.
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
        std::string user;
        bool authenticated = false;
        bool asking = false;  // waits for the greeter's answer
        bool running = false;
    };
    using Clock = std::chrono::steady_clock;

    std::unique_ptr<Worker> spawn(const std::string& service, const std::string& user, const std::string& cls,
                                  bool conversation);
    void start(Worker& w, const std::vector<std::string>& cmd, const std::vector<std::string>& env, bool profile);
    void end(std::unique_ptr<Worker>& w);
    void start_greeter();
    bool open_greeter_socket(uid_t uid, gid_t gid);
    void own_home(const passwd& pw);
    void activate_vt();

    void on_worker(std::unique_ptr<Worker>& w);
    void on_worker_gone(std::unique_ptr<Worker>& w);
    void on_client();
    void handle(const Request& r);
    void reply(const std::string& body);
    void drop_client();
    void start_pending();

    Config config_;
    std::unique_ptr<Worker> greeter_, pending_, session_;
    int signal_fd_ = -1, listen_fd_ = -1, client_fd_ = -1;
    std::string socket_path_, client_buffer_;
    // A started session waits for the greeter to go.
    std::optional<std::pair<std::vector<std::string>, std::vector<std::string>>> to_start_;
    std::optional<Clock::time_point> greeter_deadline_, greeter_restart_;
    std::vector<Clock::time_point> greeter_starts_;
    bool quitting_ = false;
};

} // namespace atrium::login
