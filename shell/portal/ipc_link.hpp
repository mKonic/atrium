#pragma once
// A connection of atrium-portal's own to the compositor's IPC socket, for a
// remote control or input capture session: the compositor ends the session
// when this connection closes, and hands libei's socket over it (SCM_RIGHTS,
// which the shell's QLocalSocket can't take). Requests wait for their reply.

#include <QJsonObject>
#include <QString>

#include <optional>

namespace atrium {

class IpcLink {
public:
    explicit IpcLink(const QString& path);
    ~IpcLink();
    IpcLink(const IpcLink&) = delete;
    IpcLink& operator=(const IpcLink&) = delete;

    bool ok() const { return fd_ >= 0; }
    // The reply's "result" when "ok", nothing otherwise. `fd`: one sent
    // along with the reply (-1 none), the caller's to close.
    std::optional<QJsonValue> request(const QJsonObject& req, int* fd = nullptr);

private:
    int fd_ = -1;
    QByteArray in_;
};

} // namespace atrium
