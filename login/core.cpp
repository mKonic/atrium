#include "core.hpp"

#include "ini_core.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace atrium::login {

using nlohmann::json;

std::optional<Request> parse_request(std::string_view text) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string())
        return std::nullopt;
    const std::string type = j["type"];
    Request r;
    auto strings = [&](const char* key, std::vector<std::string>* out) {
        if (!j.contains(key))
            return true;
        if (!j[key].is_array())
            return false;
        for (const json& v : j[key]) {
            if (!v.is_string())
                return false;
            out->push_back(v);
        }
        return true;
    };
    if (type == "create_session") {
        if (!j.contains("username") || !j["username"].is_string())
            return std::nullopt;
        r.type = Request::Type::CreateSession;
        r.username = j["username"];
    } else if (type == "post_auth_message_response") {
        r.type = Request::Type::PostAuthResponse;
        if (j.contains("response") && j["response"].is_string())
            r.response = j["response"].get<std::string>();
    } else if (type == "start_session") {
        r.type = Request::Type::StartSession;
        if (!strings("cmd", &r.cmd) || !strings("env", &r.env) || r.cmd.empty())
            return std::nullopt;
    } else if (type == "cancel_session") {
        r.type = Request::Type::CancelSession;
    } else {
        return std::nullopt;
    }
    return r;
}

std::string success() { return json{{"type", "success"}}.dump(); }

std::string error(bool auth, std::string_view description) {
    return json{{"type", "error"}, {"error_type", auth ? "auth_error" : "error"}, {"description", description}}.dump();
}

std::string auth_message(AuthKind kind, std::string_view text) {
    static constexpr const char* kKinds[] = {"visible", "secret", "info", "error"};
    return json{{"type", "auth_message"}, {"auth_message_type", kKinds[int(kind)]}, {"auth_message", text}}.dump();
}

std::string frame(std::string_view body) {
    const auto len = uint32_t(body.size());
    std::string out(4, '\0');
    std::memcpy(out.data(), &len, 4);
    out += body;
    return out;
}

std::vector<std::string> unframe(std::string& buffer, bool* bad, uint32_t limit) {
    std::vector<std::string> out;
    *bad = false;
    size_t at = 0;
    while (buffer.size() - at >= 4) {
        uint32_t len;
        std::memcpy(&len, buffer.data() + at, 4);
        if (len > limit) {
            *bad = true;
            break;
        }
        if (buffer.size() - at - 4 < len)
            break;
        out.emplace_back(buffer, at + 4, len);
        at += 4 + len;
    }
    buffer.erase(0, at);
    return out;
}

std::vector<std::string> split_command(std::string_view line) {
    std::vector<std::string> out;
    std::string word;
    bool in_word = false;
    char quote = 0;
    for (char c : line) {
        if (quote) {
            if (c == quote)
                quote = 0;
            else
                word += c;
        } else if (c == '"' || c == '\'') {
            quote = c;
            in_word = true;
        } else if (c == ' ' || c == '\t') {
            if (in_word)
                out.push_back(std::move(word));
            word.clear();
            in_word = false;
        } else {
            word += c;
            in_word = true;
        }
    }
    if (in_word)
        out.push_back(std::move(word));
    return out;
}

Config parse_config(std::string_view ini) {
    Config c;
    if (auto v = ini::get(ini, "login", "vt")) {
        int n = 0;
        auto [p, ec] = std::from_chars(v->data(), v->data() + v->size(), n);
        if (ec == std::errc() && p == v->data() + v->size() && n >= 1 && n <= 63)
            c.vt = n;
    }
    if (auto v = ini::get(ini, "login", "greeter_user"); v && !v->empty())
        c.greeter_user = *v;
    if (auto v = ini::get(ini, "login", "greeter"); v && !v->empty())
        c.greeter_command = *v;
    if (auto v = ini::get(ini, "login", "source_profile"))
        c.source_profile = *v != "false" && *v != "no" && *v != "0";
    if (auto v = ini::get(ini, "autologin", "user"))
        c.autologin_user = *v;
    if (auto v = ini::get(ini, "autologin", "session"); v && !v->empty())
        c.autologin_command = *v;
    return c;
}

std::vector<std::string> session_argv(const std::vector<std::string>& cmd, bool source_profile) {
    std::vector<std::string> argv = {"/bin/sh", "-c"};
    argv.push_back(source_profile ? "[ -f /etc/profile ] && . /etc/profile; "
                                    "[ -f \"$HOME/.profile\" ] && . \"$HOME/.profile\"; exec \"$@\""
                                  : "exec \"$@\"");
    argv.push_back("atrium-session");  // $0
    argv.insert(argv.end(), cmd.begin(), cmd.end());
    return argv;
}

std::vector<std::string> session_env(const std::vector<std::string>& pam_env, const Account& account,
                                     const std::vector<std::string>& requested) {
    std::vector<std::string> out;
    auto put = [&out](const std::string& entry) {
        const size_t eq = entry.find('=');
        if (eq == std::string::npos || eq == 0)
            return;
        const std::string_view key(entry.data(), eq + 1);
        std::erase_if(out, [&](const std::string& e) { return e.starts_with(key); });
        out.push_back(entry);
    };
    put("PATH=/usr/local/sbin:/usr/local/bin:/usr/bin");
    for (const std::string& e : pam_env)
        put(e);
    put("HOME=" + account.home);
    put("SHELL=" + (account.shell.empty() ? std::string("/bin/sh") : account.shell));
    put("USER=" + account.name);
    put("LOGNAME=" + account.name);
    // The greeter is another (unprivileged) user: nothing that changes what
    // the session loads, or which logind session it is.
    static constexpr std::string_view kKept[] = {"XDG_SESSION_ID=", "XDG_RUNTIME_DIR=", "XDG_SEAT=", "XDG_VTNR=",
                                                 "HOME=", "USER=", "LOGNAME=", "SHELL="};
    for (const std::string& e : requested)
        if (!e.starts_with("LD_") && std::ranges::none_of(kKept, [&](std::string_view k) { return e.starts_with(k); }))
            put(e);
    return out;
}

int greeter_vt(int configured, const std::vector<int>& session_vts, int free_vt) {
    if (std::ranges::find(session_vts, configured) == session_vts.end())
        return configured;
    if (free_vt > 0 && std::ranges::find(session_vts, free_vt) == session_vts.end())
        return free_vt;
    return 0;
}

std::optional<Control> parse_control(std::string_view line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
        line.remove_suffix(1);
    if (line == "switch-to-greeter")
        return Control::SwitchToGreeter;
    return std::nullopt;
}

} // namespace atrium::login
