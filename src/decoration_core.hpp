#pragma once
// When a client taking back its choice of decoration takes effect.
//
// Destroying the xdg-decoration (or KDE server-decoration) object means the
// client has no say any more, and atrium's title bar goes. But the frame is
// only restyled with the window's next commit, never at the destroy itself:
// Qt destroys the object on the way out, just before the window, and
// restyling then took the title bar off the frame that is remembered for the
// app (Server::remember_placement), so a Qt app opened 30 px shorter every
// time (Dolphin, 2026-10-14). A window that carries on without the object
// commits again, and is restyled then.
namespace atrium {

class DecorationLatch {
public:
    // The client destroyed its decoration object.
    void dropped() { due_ = true; }
    // The window committed; true (once) if it is to be restyled now.
    bool commit(bool mapped) {
        if (!mapped || !due_)
            return false;
        due_ = false;
        return true;
    }
    // The window went: nothing is owed to it.
    void unmapped() { due_ = false; }

private:
    bool due_ = false;
};

} // namespace atrium
