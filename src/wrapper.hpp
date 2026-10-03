#pragma once
// Crash recovery, as KWin's kwin_wayland_wrapper: atrium starts as a small
// parent that owns the Wayland socket (and its lock) and runs the compositor
// as its child, handing it the listening socket. A child that crashes is
// started again on the same socket, so apps that reconnect (Qt 6 with
// QT_WAYLAND_RECONNECT) come back to it; one that exits cleanly ends the
// session.
namespace atrium {

// Runs the parent until the session ends; its exit status. `argv` restarts
// the compositor (this binary, the same arguments).
int run_wrapper(char** argv);

// The socket the parent handed this compositor, or -1 (not wrapped); its
// name in `*name`. Takes them out of the environment.
int take_wrapped_socket(const char** name);

// Crashes in a row (newest last, steady-clock seconds) that are too many to
// try again: three within a minute.
bool too_many_crashes(const double* times, int count, double now);

} // namespace atrium
