#pragma once
// Avahi's client library on Qt's event loop: an AvahiPoll whose watches are
// QSocketNotifiers and whose timeouts are QTimers.

#include <avahi-common/watch.h>

namespace atrium::phonelink {

const AvahiPoll* qtAvahiPoll();

} // namespace atrium::phonelink
