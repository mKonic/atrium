#pragma once
#include "wlr.hpp"

#include <string>
#include <sys/types.h>

namespace atrium {

class Server;

// The desktop shell (bar, dock, launcher...) as a child atrium keeps
// running: started with the session, restarted when it dies, backing off
// if it keeps dying right away.
//
// session.shell picks it: "builtin" runs atrium's own shell (atrium-shell),
// anything else is a command, empty or "none" runs nothing.
class ShellProcess {
public:
    explicit ShellProcess(Server& server);
    ~ShellProcess();
    ShellProcess(const ShellProcess&) = delete;
    ShellProcess& operator=(const ShellProcess&) = delete;

    void start();
    void stop();
    void restart();

    // The command start() would run; empty when there is nothing to run.
    std::string command() const;

private:
    void exited(int status);
    void schedule(int delay_ms);

    Server& server_;
    pid_t pid_ = -1;
    double started_ms_ = 0;
    int quick_failures_ = 0;
    static constexpr int kSafeAfter = 3;  // quick failures before safe mode
    int pipe_[2] = {-1, -1};  // the SIGCHLD handler reports the shell's exit here
    wl_event_source* pipe_source_ = nullptr;
    wl_event_source* retry_ = nullptr;
};

} // namespace atrium
