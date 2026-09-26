#pragma once
// This end's newest clips for the exchange on connecting: the clipboard's
// last few, current first, with when each was copied. Kept in
// $XDG_STATE_HOME/atrium/clipsync so a restart still knows the times; the
// first run starts from cliphist (times unknown). Clips from the phone also
// go into cliphist, so the Super+V picker has them.

#include "clipsync_core.hpp"

#include <QString>

#include <vector>

namespace atrium::clipsync {

class RecentClips {
public:
    RecentClips();

    // Current first.
    const std::vector<Clip>& recent() const { return clips_; }
    // Became the clipboard (copied here, or taken from the phone).
    void current(const Clip& c);
    // From the phone's history: goes in by its time, behind the current clip.
    void older(const Clip& c);
    // Adds it to cliphist (on top, or moved there).
    void toCliphist(const Clip& c) const;

private:
    void seed();
    void save() const;

    std::vector<Clip> clips_;
    QString file_, cliphist_;
};

} // namespace atrium::clipsync
