#pragma once
// atrium-login's pure parts: greetd's IPC (which the greeter speaks, so
// greetd stays a drop-in fallback), the config, and how a session is run.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atrium::login {

// --- greetd IPC: native-endian u32 length, then JSON ---

struct Request {
    enum class Type { CreateSession, PostAuthResponse, StartSession, CancelSession };
    Type type{};
    std::string username;                 // create_session
    std::optional<std::string> response;  // post_auth_message_response (absent: none)
    std::vector<std::string> cmd, env;    // start_session
};
std::optional<Request> parse_request(std::string_view json);

enum class AuthKind { Visible, Secret, Info, Error };
std::string success();
// auth: wrong credentials (the greeter shakes), else something broke.
std::string error(bool auth, std::string_view description);
std::string auth_message(AuthKind kind, std::string_view text);

std::string frame(std::string_view body);
// The whole messages at the front of `buffer`, taken out of it. A message
// claiming more than `limit` bytes sets *bad (the peer is dropped).
std::vector<std::string> unframe(std::string& buffer, bool* bad, uint32_t limit = 1 << 20);

// --- the config: /etc/atrium/login.conf ---

struct Config {
    int vt = 1;
    std::string greeter_user = "atrium-greeter";
    std::string greeter_command = "atrium --greeter";
    // Logged in straight away at boot (once), then the greeter.
    std::string autologin_user;
    std::string autologin_command = "atrium";
    // /etc/profile and ~/.profile before the session, as a login shell would.
    bool source_profile = true;
};
// Unknown keys and bad values keep the defaults.
Config parse_config(std::string_view ini);

// --- running a session ---

// What the session child execs: `cmd` through /bin/sh, with the profiles
// sourced first when asked.
std::vector<std::string> session_argv(const std::vector<std::string>& cmd, bool source_profile);
// A shell command line split as the config's commands are written (spaces,
// with "double" or 'single' quotes).
std::vector<std::string> split_command(std::string_view line);

struct Account {
    std::string name, home, shell;
};
// The session's environment: PAM's (pam_systemd's XDG_* among it), then
// the account's basics, then what the greeter asked for (later entries win),
// except LD_* and what logind and the account decide.
std::vector<std::string> session_env(const std::vector<std::string>& pam_env, const Account& account,
                                     const std::vector<std::string>& requested);

// --- several sessions, one VT each ---

// The VT a new greeter takes: the configured one while no session is on it,
// else `free_vt` (the kernel's first unused one), else none (0).
int greeter_vt(int configured, const std::vector<int>& session_vts, int free_vt);

// The VT a session logged in from the greeter on `greeter` starts on: a free
// one, so it's in front before the greeter goes and the greeter's last frame
// stays up until the session draws (as GDM keeps them apart). With none free,
// the greeter's own, once the greeter has quit (greetd's way).
int session_vt(int greeter, const std::vector<int>& session_vts, int free_vt);

// What a logged-in session may ask of the daemon on its control socket, a
// line each: "switch-to-greeter" (a greeter on a VT of its own, the asking
// session left running), or "displays [...]" (its displays as JSON, for the
// greeter to come up the same way: no modeset when logging in), or "logout"
// (the greeter in front before the session goes, so its VT going back to
// text is never seen).
enum class Control { SwitchToGreeter, Displays, Logout };
struct ControlRequest {
    Control kind;
    std::string payload;  // Displays: the JSON array, as sent
};
std::optional<ControlRequest> parse_control(std::string_view line);
// The longest control line taken (a displays line with every screen).
constexpr size_t kControlLineMax = 64 * 1024;

} // namespace atrium::login
