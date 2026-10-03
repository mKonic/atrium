#pragma once
// The pure parts of atrium-portal's backends, apart from Qt and D-Bus.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atrium::portal {

// What the Email portal asks to write, as a mailto: link (RFC 6068).
struct Email {
    std::vector<std::string> to, cc, bcc;
    std::string subject, body;
    bool has_subject = false, has_body = false;
};
std::string mailto(const Email& e);

// A portal notification's priority ("low", "normal", "high", "urgent") as
// a freedesktop urgency: 0 low, 1 normal, 2 critical. Only "urgent" is
// critical, the kind that stays until dismissed.
uint8_t urgency(std::string_view priority);

} // namespace atrium::portal
