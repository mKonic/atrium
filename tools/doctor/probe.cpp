#include "probe.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

extern char** environ;

namespace fs = std::filesystem;

namespace atrium::doctor {

std::string capture(const std::vector<std::string>& argv, int* status, bool errors) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0)
        return {};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    if (errors)
        posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    else
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    std::vector<char*> args;
    for (const std::string& a : argv)
        args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid;
    const int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    std::string out;
    if (rc == 0) {
        char buf[4096];
        for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;)
            out.append(buf, size_t(n));
        int st = 0;
        waitpid(pid, &st, 0);
        if (status)
            *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    } else if (status) {
        *status = -1;
    }
    close(fds[0]);
    return out;
}

bool run(const std::vector<std::string>& argv, const std::string& dir, const std::vector<std::string>& env) {
    std::string line;
    for (const std::string& a : argv)
        line += (line.empty() ? "" : " ") + a;
    std::printf("\n$ %s\n", line.c_str());
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        if (!dir.empty() && chdir(dir.c_str()) != 0)
            _exit(127);
        for (const std::string& e : env)
            putenv(const_cast<char*>(e.c_str()));
        std::vector<char*> args;
        for (const std::string& a : argv)
            args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at);
        std::string l = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        if (!l.empty())
            out.push_back(std::move(l));
        if (nl == std::string::npos)
            break;
        at = nl + 1;
    }
    return out;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    return s;
}

std::string owner(const std::string& file) {
    return trim(capture({"pacman", "-Qqo", file}));
}

Process process(int pid) {
    std::error_code ec;
    std::string exe = fs::read_symlink("/proc/" + std::to_string(pid) + "/exe", ec).string();
    if (ec)
        return {};
    Process p{pid, exe, false};
    if (p.exe.ends_with(" (deleted)")) {
        p.exe.resize(p.exe.size() - 10);
        p.stale = true;
    }
    return p;
}

std::vector<Process> processes(const std::string& name) {
    std::vector<Process> out;
    const uid_t me = getuid();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/proc", ec)) {
        const std::string id = e.path().filename().string();
        if (id.find_first_not_of("0123456789") != std::string::npos)
            continue;
        struct stat st {};
        if (stat(e.path().c_str(), &st) != 0 || st.st_uid != me)
            continue;
        const Process p = process(std::atoi(id.c_str()));
        if (p.pid && (p.exe == "/usr/bin/" + name || p.exe == "/usr/lib/" + name))
            out.push_back(p);
    }
    return out;
}

long long age(int pid) {
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string text((std::istreambuf_iterator<char>(stat)), {});
    // The 22nd field, counted after the command's closing parenthesis.
    const size_t close = text.rfind(')');
    if (close == std::string::npos)
        return -1;
    std::istringstream rest(text.substr(close + 2));
    std::string field;
    for (int i = 3; i <= 22 && rest >> field; ++i)
        ;
    double uptime = 0;
    std::ifstream("/proc/uptime") >> uptime;
    const long long ticks = std::atoll(field.c_str());
    return (long long)uptime - ticks / sysconf(_SC_CLK_TCK);
}

std::string environment(int pid, const std::string& key) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/environ");
    std::string entry;
    while (std::getline(f, entry, '\0'))
        if (entry.starts_with(key + "="))
            return entry.substr(key.size() + 1);
    return {};
}

std::string unit(int pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/cgroup");
    std::string line;
    std::getline(f, line);
    return line.substr(line.rfind('/') == std::string::npos ? 0 : line.rfind('/') + 1);
}

int bus_owner(const std::string& name) {
    const std::string out = capture({"busctl", "--user", "--timeout=3", "call", "org.freedesktop.DBus",
                                     "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetConnectionUnixProcessID",
                                     "s", name});
    // "u 70485"
    return out.starts_with("u ") ? std::atoi(out.c_str() + 2) : 0;
}

std::vector<std::string> journal(const std::string& identifier, long long since, const std::string& priority) {
    std::vector<std::string> argv = {"journalctl", "--user", "--no-pager", "-q", "-o", "cat",
                                     "-t", identifier, "--since", "@" + std::to_string(since)};
    if (!priority.empty())
        argv.insert(argv.end(), {"-p", priority});
    return lines(capture(argv));
}

} // namespace atrium::doctor
