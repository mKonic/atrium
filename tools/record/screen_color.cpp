#include "screen_color.hpp"

#include <nlohmann/json.hpp>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace atrium::record {

namespace {

int connect_to(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path)
        return -1;
    std::strcpy(addr.sun_path, path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        close(fd);
        fd = -1;
    }
    return fd;
}

// atriumctl's: $ATRIUM_SOCKET, else the one live atrium socket.
int connect_atrium() {
    if (const char* s = std::getenv("ATRIUM_SOCKET"); s && *s)
        return connect_to(s);
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime)
        return -1;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(runtime, ec)) {
        const std::string name = e.path().filename();
        if (name.starts_with("atrium.") && name.ends_with(".sock"))
            if (int fd = connect_to(e.path()); fd >= 0)
                return fd;
    }
    return -1;
}

} // namespace

ScreenColorWatch::~ScreenColorWatch() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

bool ScreenColorWatch::query(ScreenColor& out) {
    const int fd = connect_atrium();
    if (fd < 0)
        return false;
    const std::string req = "{\"cmd\":\"outputs\"}\n";
    bool ok = write(fd, req.data(), req.size()) == ssize_t(req.size());
    std::string buf;
    while (ok && buf.find('\n') == std::string::npos) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 1000) <= 0)
            break;
        char chunk[8192];
        const ssize_t n = read(fd, chunk, sizeof chunk);
        if (n <= 0)
            break;
        buf.append(chunk, size_t(n));
    }
    close(fd);
    const size_t nl = buf.find('\n');
    if (nl == std::string::npos)
        return false;
    try {
        const nlohmann::json reply = nlohmann::json::parse(buf.substr(0, nl));
        if (!reply.value("ok", false))
            return false;
        for (const auto& o : reply["result"]) {
            if (o.value("name", "") != output_)
                continue;
            out.hdr = o.value("hdr_active", false);
            if (o.contains("night_light_gain") && o["night_light_gain"].size() == 3)
                for (int i = 0; i < 3; i++)
                    out.night[i] = o["night_light_gain"][size_t(i)].get<float>();
            return true;
        }
    } catch (const nlohmann::json::exception&) {
    }
    return false;
}

bool ScreenColorWatch::start(const std::string& output) {
    output_ = output;
    ScreenColor c;
    if (!query(c))
        return false;
    {
        std::lock_guard g(lock_);
        color_ = c;
    }
    thread_ = std::thread([this] {
        while (!stop_) {
            for (int i = 0; i < 10 && !stop_; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ScreenColor next;
            if (!stop_ && query(next)) {
                std::lock_guard g(lock_);
                color_ = next;
            }
        }
    });
    return true;
}

ScreenColor ScreenColorWatch::get() const {
    std::lock_guard g(lock_);
    return color_;
}

} // namespace atrium::record
