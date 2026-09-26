#pragma once
// An RFCOMM connection to a service on a paired device: an SDP lookup of its
// channel on the device itself (BlueZ's own list of a device's services is
// read once, at pairing, so a service added later isn't in it), then the
// socket, encrypted. Blocking: run it off the main thread.

#include <string>
#include <string_view>

namespace atrium::clipsync {

// The connected socket, or -1 with `error` saying why.
int connectService(const std::string& address, std::string_view uuid, std::string& error);

} // namespace atrium::clipsync
