#pragma once
// Changing your own password the ordinary way: the system's setuid passwd,
// driven through a pseudo-terminal (it reads from a terminal, not stdin),
// answering its prompts. PAM checks the current password and the rules for
// the new one, exactly as at a terminal.

#include <string>

namespace atrium::passwd {

struct Result {
    bool ok = false;
    std::string message;  // on failure, what passwd said, for showing
};

// `program` is passwd (tests pass a stand-in). Blocks until it is done or
// `timeout_ms` runs out.
Result change(const std::string& program, const std::string& current, const std::string& next,
              int timeout_ms = 20000);

// Account names as useradd takes them: a lowercase letter, then lowercase
// letters, digits, "-" or "_", 32 at most.
bool valid_user_name(const std::string& name);
// "Jane Q. Doe" → "janedoe": what macOS fills in from the full name.
std::string suggest_user_name(const std::string& real_name);
// A password hashed for /etc/shadow with the system's preferred method
// (yescrypt today); empty if hashing failed.
std::string hash(const std::string& password);

} // namespace atrium::passwd
