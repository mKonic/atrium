#include "child_watch.hpp"

#include <csignal>
#include <iterator>
#include <unistd.h>

namespace atrium {

namespace {

// Read by the SIGCHLD handler, which may only do async-signal-safe things.
struct Slot {
    volatile sig_atomic_t pid = -1;
    volatile sig_atomic_t fd = -1;
};
Slot g_slots[8];

} // namespace

int child_watch_add(int fd) {
    for (int i = 0; i < int(std::size(g_slots)); ++i)
        if (g_slots[i].fd < 0) {
            g_slots[i].pid = -1;
            g_slots[i].fd = fd;
            return i;
        }
    return -1;
}

void child_watch_set(int slot, pid_t pid) {
    if (slot >= 0)
        g_slots[slot].pid = pid;
}

void child_watch_remove(int slot) {
    if (slot < 0)
        return;
    g_slots[slot].pid = -1;
    g_slots[slot].fd = -1;
}

void report_child_exit(pid_t pid, int status) {
    for (const Slot& s : g_slots)
        if (s.pid == pid && s.fd >= 0) {
            [[maybe_unused]] ssize_t n = write(s.fd, &status, sizeof status);
        }
}

} // namespace atrium
