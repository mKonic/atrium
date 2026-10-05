#pragma once
// Children atrium keeps an eye on (the shell, the lock screen): the SIGCHLD
// handler writes each one's exit status to the fd watching it.
#include <sys/types.h>

namespace atrium {

// A slot for watching one child at a time through `fd` (-1 if all are
// taken); child_watch_set names the child, child_watch_remove frees it.
int child_watch_add(int fd);
void child_watch_set(int slot, pid_t pid);
void child_watch_remove(int slot);
// From the SIGCHLD handler: async-signal-safe.
void report_child_exit(pid_t pid, int status);

} // namespace atrium
