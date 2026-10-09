#include "ipc_link.hpp"

#include <QJsonDocument>

#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace atrium {

namespace {

constexpr int kTimeoutMs = 3000;  // the compositor answers at once, or not at all

} // namespace

IpcLink::IpcLink(const QString& path) {
    const QByteArray p = path.toLocal8Bit();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (p.isEmpty() || size_t(p.size()) >= sizeof addr.sun_path)
        return;
    std::memcpy(addr.sun_path, p.constData(), size_t(p.size()));
    fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ >= 0 && ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        close(fd_);
        fd_ = -1;
    }
}

IpcLink::~IpcLink() {
    if (fd_ >= 0)
        close(fd_);
}

std::optional<QJsonValue> IpcLink::request(const QJsonObject& req, int* fd) {
    if (fd)
        *fd = -1;
    if (fd_ < 0)
        return std::nullopt;
    const QByteArray line = QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n';
    if (::send(fd_, line.constData(), size_t(line.size()), MSG_NOSIGNAL) != line.size())
        return std::nullopt;
    // Read up to the reply's newline, taking an fd from the first bytes.
    while (!in_.contains('\n')) {
        pollfd p{fd_, POLLIN, 0};
        if (poll(&p, 1, kTimeoutMs) <= 0)
            return std::nullopt;
        char buf[4096];
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
        iovec iov{buf, sizeof buf};
        msghdr m{};
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        m.msg_control = control;
        m.msg_controllen = sizeof control;
        const ssize_t n = recvmsg(fd_, &m, MSG_CMSG_CLOEXEC);
        if (n <= 0)
            return std::nullopt;
        for (cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int got;
                std::memcpy(&got, CMSG_DATA(c), sizeof got);
                if (fd && *fd < 0)
                    *fd = got;
                else
                    close(got);
            }
        in_.append(buf, int(n));
    }
    const int nl = in_.indexOf('\n');
    const QJsonObject reply = QJsonDocument::fromJson(in_.left(nl)).object();
    in_.remove(0, nl + 1);
    if (!reply.value("ok").toBool())
        return std::nullopt;
    return reply.value("result");
}

} // namespace atrium
