#pragma once
// One login session's PAM life, in a process of its own (PAM modules block
// and keep state): authenticate (asking the daemon for each answer), open
// the session, run it as the user, wait for it, close the session. The
// daemon talks to it in JSON messages over a SOCK_SEQPACKET pair:
//
//   daemon → worker  {"t":"init","service","user","class","vt","auth"}
//                    {"t":"answer","text"?}   {"t":"start","cmd","env","profile"}
//                    {"t":"stop"}             (or closing the socket)
//   worker → daemon  {"t":"ask","kind","text"}   {"t":"auth","ok","error"?}
//                    {"t":"started","pid"}      {"t":"done"}
#include <nlohmann/json.hpp>

#include <optional>

namespace atrium::login {

bool send_message(int fd, const nlohmann::json& m);
// Blocks; nothing on EOF or a broken message.
std::optional<nlohmann::json> receive_message(int fd);

[[noreturn]] void run_worker(int fd);

} // namespace atrium::login
