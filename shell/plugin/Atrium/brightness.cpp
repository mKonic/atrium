#include "brightness.hpp"
#include "compositor.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <thread>

namespace atrium {

namespace {

int readInt(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed().toInt() : 0;
}

// DDC/CI straight on the monitor's I2C bus. Each ddcutil run searches every
// screen first, and on NVIDIA that holds the display driver long enough to
// stall the compositor for a fifth of a second; one message is a few bytes.
constexpr int kDdcAddress = 0x37;
constexpr uint8_t kBrightness = 0x10;

int openBus(int bus) {
    const int fd = ::open(QString("/dev/i2c-%1").arg(bus).toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
    if (fd >= 0 && ioctl(fd, I2C_SLAVE, kDdcAddress) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// The message with its checksum, which covers the destination address too.
template <size_t N>
std::array<uint8_t, N + 1> packet(const std::array<uint8_t, N>& body) {
    std::array<uint8_t, N + 1> out{};
    uint8_t sum = kDdcAddress << 1;
    for (size_t i = 0; i < N; ++i) {
        out[i] = body[i];
        sum ^= body[i];
    }
    out[N] = sum;
    return out;
}

bool ddcWrite(int bus, int value) {
    const int fd = openBus(bus);
    if (fd < 0)
        return false;
    const auto msg = packet<6>({0x51, 0x84, 0x03, kBrightness, uint8_t(value >> 8), uint8_t(value & 0xff)});
    const bool ok = ::write(fd, msg.data(), msg.size()) == ssize_t(msg.size());
    ::close(fd);
    return ok;
}

} // namespace

Brightness::Brightness(QObject* parent) : QObject(parent) {
    // A laptop panel.
    const QStringList panels = QDir("/sys/class/backlight").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (!panels.isEmpty()) {
        backlight_ = "/sys/class/backlight/" + panels.first();
        backlightMax_ = readInt(backlight_ + "/max_brightness");
        if (backlightMax_ > 0)
            value_ = readInt(backlight_ + "/brightness") * 100 / backlightMax_;
    }
    // Test boxes leave the real monitors alone.
    if (qEnvironmentVariableIsEmpty("ATRIUM_NO_DDC"))
        ddcutil_ = QStandardPaths::findExecutable("ddcutil");
    // The Brightness setting changed (in Settings): the screens follow. atrium
    // itself sets them to it at login.
    seen_ = setting();
    connect(Compositor::instance(), &Compositor::settingsChanged, this, [this] { applySetting(); });
    connect(Compositor::instance(), &Compositor::outputsChanged, this, [this] {
        if (hdr() != hdr_) {
            hdr_ = hdr();
            emit changed();
        }
    });
    hdr_ = hdr();
    detect();
}

bool Brightness::available() const {
    return !buses_.empty() || !backlight_.isEmpty() || hdr_;
}

int Brightness::value() const {
    // In HDR the screen runs at full backlight; what reads as brightness is
    // how bright atrium draws everything that isn't HDR (as KWin does).
    if (hdr_)
        for (const QVariant& o : Compositor::instance()->outputs())
            if (o.toMap().value("hdr").toBool())
                return o.toMap().value("sdr_brightness").toInt();
    return value_;
}

bool Brightness::hdr() const {
    for (const QVariant& o : Compositor::instance()->outputs())
        if (o.toMap().value("hdr").toBool())
            return true;
    return false;
}

std::optional<int> Brightness::setting() const {
    const QVariant v = Compositor::instance()->settings().value("displays.brightness");
    return v.isValid() ? std::optional<int>(v.toInt()) : std::nullopt;
}

void Brightness::applySetting() {
    const auto v = setting();
    // The first time it arrives is no change; nor is the Control Center's
    // own slider, let go where the screens already are.
    const bool changed = v && seen_ && *v != *seen_;
    seen_ = v;
    if (changed && (!buses_.empty() || !backlight_.isEmpty()) && *v != value_)
        setBacklight(*v);
}

// Which monitors answer DDC/CI, once: ddcutil finds their buses. Takes a
// second or two; nothing waits on it.
void Brightness::detect() {
    if (ddcutil_.isEmpty())
        return;
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p] {
        static const QRegularExpression bus(R"(I2C bus:\s*/dev/i2c-(\d+))");
        auto it = bus.globalMatch(QString::fromUtf8(p->readAllStandardOutput()));
        while (it.hasNext())
            buses_.push_back(it.next().captured(1).toInt());
        emit changed();
        readCurrent();
        p->deleteLater();
    });
    p->start(ddcutil_, {"detect", "--brief"});
}

// Where the monitor is now, once: reading back over DDC takes ddcutil's
// retries (replies come garbled on NVIDIA's I2C).
void Brightness::readCurrent() {
    if (buses_.empty())
        return;
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p] {
        // "VCP 10 C 80 100": current, max.
        const QStringList parts = QString::fromUtf8(p->readAllStandardOutput()).split(' ', Qt::SkipEmptyParts);
        if (parts.size() >= 5 && parts[4].toInt() > 0) {
            max_ = parts[4].toInt();
            value_ = parts[3].toInt() * 100 / max_;
            emit changed();
        }
        p->deleteLater();
    });
    p->start(ddcutil_, {"getvcp", "10", "--brief", "--skip-ddc-checks", "--bus", QString::number(buses_.front())});
}

void Brightness::set(int percent) {
    percent = std::clamp(percent, 0, 100);
    if (hdr_) {
        for (const QVariant& o : Compositor::instance()->outputs()) {
            const QVariantMap m = o.toMap();
            if (m.value("hdr").toBool())
                Compositor::instance()->configureOutput(m.value("name").toString(), {{"sdr_brightness", percent}});
        }
        return;
    }
    setBacklight(percent);
}

void Brightness::commit(int percent) {
    percent = std::clamp(percent, 0, 100);
    if (hdr_)
        set(percent);  // saved with the display
    else
        Compositor::instance()->setSetting("displays.brightness", percent);
}

void Brightness::setBacklight(int percent) {
    value_ = percent;
    emit changed();
    if (!backlight_.isEmpty() && backlightMax_ > 0) {
        // logind lets the session set its own backlight.
        auto call = QDBusMessage::createMethodCall("org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
                                                   "org.freedesktop.login1.Session", "SetBrightness");
        call << QString("backlight") << backlight_.section('/', -1) << uint(percent * backlightMax_ / 100);
        QDBusConnection::systemBus().asyncCall(call);
    }
    if (buses_.empty())
        return;
    pending_ = percent;
    writeNext();
}

void Brightness::writeNext() {
    if (busy_ || !pending_)
        return;
    const int value = *pending_ * max_ / 100;
    pending_.reset();
    busy_ = true;
    const std::vector<int> buses = buses_;
    QThreadPool::globalInstance()->start([this, buses, value] {
        for (int bus : buses)
            ddcWrite(bus, value);
        // Each write holds the display driver a moment (a frame's time on
        // NVIDIA); a slider moving sends a few a second, not one per step.
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        QMetaObject::invokeMethod(this, [this] {
            busy_ = false;
            writeNext();  // the slider moved on meanwhile
        }, Qt::QueuedConnection);
    });
}

} // namespace atrium
