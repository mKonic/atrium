#include "passwd_core.hpp"

#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <crypt.h>

namespace atrium::passwd {

namespace {

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// Why it failed: PAM's reason for turning a password down if it gave one,
// else the last thing passwd said that wasn't a prompt.
std::string last_line(const std::string& out) {
    if (size_t bad = out.find("BAD PASSWORD: "); bad != std::string::npos) {
        std::string why = out.substr(bad + 14, out.find('\n', bad) - bad - 14);
        std::erase(why, '\r');
        return why;
    }
    std::string best;
    size_t start = 0;
    while (start < out.size()) {
        size_t end = out.find('\n', start);
        if (end == std::string::npos)
            end = out.size();
        std::string line = out.substr(start, end - start);
        std::erase(line, '\r');
        const std::string l = lower(line);
        if (!line.empty() && !l.ends_with(": ") && !l.ends_with(":") && !l.starts_with("changing password"))
            best = line;
        start = end + 1;
    }
    if (best.starts_with("passwd: "))
        best.erase(0, 8);
    return best;
}

} // namespace

Result change(const std::string& program, const std::string& current, const std::string& next, int timeout_ms) {
    int fd = -1;
    const pid_t pid = forkpty(&fd, nullptr, nullptr, nullptr);
    if (pid < 0)
        return {false, "couldn't start passwd"};
    if (pid == 0) {
        setenv("LC_ALL", "C", 1);  // prompts to match below, whatever the locale
        execl(program.c_str(), program.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    std::string out;       // everything, for the error
    std::string pending;   // since the last answer, to spot the next prompt
    int answered_new = 0;
    int status = 0;
    bool reaped = false;
    bool answered_current = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 deadline - std::chrono::steady_clock::now()).count());
        if (left <= 0) {
            kill(pid, SIGKILL);
            break;
        }
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, std::min(left, 200)) <= 0) {
            if (waitpid(pid, &status, WNOHANG) == pid) {  // gone without a last word
                reaped = true;
                break;
            }
            continue;
        }
        char buf[512];
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0)
            break;  // EIO: the child closed the terminal
        out.append(buf, size_t(n));
        pending.append(buf, size_t(n));
        const std::string l = lower(pending);
        auto answer = [&](const std::string& text) {
            const std::string line = text + "\n";
            (void)!write(fd, line.data(), line.size());
            pending.clear();
        };
        if (!l.ends_with(": ") && !l.ends_with(":"))
            continue;
        const bool retype = l.find("retype") != std::string::npos || l.find("again") != std::string::npos;
        if (!answered_current && (l.find("current") != std::string::npos || l.find("old") != std::string::npos)) {
            answered_current = true;
            answer(current);
        } else if (retype) {
            answer(next);
        } else if (l.find("password") != std::string::npos && answered_new == 0) {
            ++answered_new;
            answer(next);
        } else if (l.find("password") != std::string::npos) {
            // Asked for a new one again: it turned the first down. Saying
            // the same thing twice won't help.
            kill(pid, SIGTERM);
        }
    }
    if (!reaped)
        waitpid(pid, &status, 0);
    close(fd);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return {true, {}};
    // PAM's words for a wrong current password.
    const std::string all = lower(out);
    if (all.find("authentication token manipulation") != std::string::npos ||
        all.find("authentication failure") != std::string::npos)
        return {false, "Your current password is incorrect."};
    std::string why = last_line(out);
    if (why.empty())
        return {false, "The password wasn't changed."};
    why[0] = char(std::toupper(static_cast<unsigned char>(why[0])));
    if (!why.ends_with('.'))
        why += '.';
    return {false, why};
}

bool valid_user_name(const std::string& name) {
    if (name.empty() || name.size() > 32 || !(name[0] >= 'a' && name[0] <= 'z'))
        return false;
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

std::string suggest_user_name(const std::string& real_name) {
    // First and last word, lowercased, letters and digits only.
    std::vector<std::string> words;
    std::string word;
    for (char c : real_name + " ") {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!word.empty())
                words.push_back(word);
            word.clear();
        } else if (std::isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 128) {
            word += char(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    // Only words that can start a name ("3 Musketeers" → "musketeers").
    std::erase_if(words, [](const std::string& w) { return w.empty() || !(w[0] >= 'a' && w[0] <= 'z'); });
    std::string out = words.empty() ? "" : words.size() == 1 ? words[0] : words.front() + words.back();
    while (!out.empty() && !(out[0] >= 'a' && out[0] <= 'z'))
        out.erase(0, 1);
    return out.substr(0, 32);
}

std::string hash(const std::string& password) {
    char* salt = crypt_gensalt_ra(nullptr, 0, nullptr, 0);  // the system's default prefix
    if (!salt)
        return {};
    void* data = nullptr;
    int size = 0;
    const char* h = crypt_ra(password.c_str(), salt, &data, &size);
    std::string out = h && h[0] != '*' ? h : "";
    free(data);
    free(salt);
    return out;
}

} // namespace atrium::passwd
