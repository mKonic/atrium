#include "brightness.hpp"
#include "compositor.hpp"
#include "ddc_core.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QThreadPool>
#include <QTimer>

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
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

// DDC/CI straight on the monitor's I2C bus, as ddcutil does it. Each ddcutil
// run searches every screen first, and on NVIDIA that holds the display
// driver long enough to stall the compositor for a fifth of a second; one
// message is a few bytes.
int openBus(int bus) {
    return ::open(QString("/dev/i2c-%1").arg(bus).toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
}

// One message to or from `address`, as ddcutil's default (ioctl) I/O.
bool transfer(int fd, uint16_t address, bool read, uint8_t* bytes, size_t length) {
    i2c_msg msg{address, uint16_t(read ? I2C_M_RD : 0), uint16_t(length), bytes};
    i2c_rdwr_ioctl_data data{&msg, 1};
    return ioctl(fd, I2C_RDWR, &data) == 1;
}

bool ddcWrite(int bus, int value) {
    const int fd = openBus(bus);
    if (fd < 0)
        return false;
    auto msg = ddc::set_request(ddc::kBrightness, uint16_t(value));
    const bool ok = transfer(fd, ddc::kAddress, false, msg.data(), msg.size());
    ::close(fd);
    return ok;
}

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Brightness from the monitor, retried as ddcutil retries: the spec's 50 ms
// between asking and reading isn't enough for every monitor (ddcutil learns
// 100 ms for the MSI on NVIDIA), and a bad exchange spoils the one straight
// after it, so each try waits longer and a failure is followed by a pause.
std::optional<ddc::Vcp> ddcRead(int fd) {
    for (int attempt = 1; attempt <= 4; ++attempt) {
        auto request = ddc::get_request(ddc::kBrightness);
        if (!transfer(fd, ddc::kAddress, false, request.data(), request.size()))
            return std::nullopt;  // nothing at 0x37
        sleepMs(50 * attempt);
        std::array<uint8_t, ddc::kReplyRead> reply{};
        ddc::Vcp vcp;
        const ddc::Reply r = transfer(fd, ddc::kAddress, true, reply.data(), reply.size())
                                 ? ddc::parse_get_reply(reply, ddc::kBrightness, vcp)
                                 : ddc::Reply::Garbled;
        if (r == ddc::Reply::Ok)
            return vcp.max > 0 ? std::optional(vcp) : std::nullopt;
        if (r == ddc::Reply::Unsupported)
            return std::nullopt;
        sleepMs(200);
    }
    return std::nullopt;
}

QString sysfsText(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
}

// The connector each bus belongs to, where the driver says (not NVIDIA's).
QHash<int, QString> busConnectors() {
    QHash<int, QString> out;
    for (const QFileInfo& c : QDir("/sys/class/drm").entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString ddcLink = QFileInfo(c.filePath() + "/ddc").symLinkTarget();
        if (!ddcLink.isEmpty())
            out.insert(ddcLink.section("i2c-", -1).toInt(), c.filePath());
    }
    return out;
}

struct Found {
    std::vector<int> buses;
    std::optional<ddc::Vcp> vcp;  // the first monitor's
};

// Every monitor that answers DDC/CI, as ddcutil detect finds them: the buses
// it would probe, with an EDID behind them, that answer for brightness.
Found findMonitors() {
    Found found;
    const QHash<int, QString> connectors = busConnectors();
    for (const QFileInfo& adapter : QDir("/sys/bus/i2c/devices").entryInfoList({"i2c-*"}, QDir::Dirs)) {
        const int bus = adapter.fileName().mid(4).toInt();
        // The PCI device above it, and its driver.
        uint32_t pciClass = 0;
        QString driver;
        for (QDir up(adapter.canonicalFilePath()); up.cdUp() && up.path() != "/sys/devices";)
            if (QFile::exists(up.filePath("class"))) {
                pciClass = sysfsText(up.filePath("class")).toUInt(nullptr, 16);
                driver = QFileInfo(up.filePath("driver")).symLinkTarget().section('/', -1);
                break;
            }
        if (ddc::ignorable_bus(sysfsText(adapter.filePath() + "/name").toStdString(), driver.toStdString(), pciClass))
            continue;
        if (connectors.contains(bus) && sysfsText(connectors[bus] + "/status") != "connected")
            continue;
        const int fd = openBus(bus);
        if (fd < 0)
            continue;
        uint8_t offset = 0;
        std::array<uint8_t, 128> edid{};
        if (transfer(fd, 0x50, false, &offset, 1) && transfer(fd, 0x50, true, edid.data(), edid.size()) &&
            ddc::edid_header(edid))
            if (const auto vcp = ddcRead(fd)) {
                found.buses.push_back(bus);
                if (!found.vcp)
                    found.vcp = vcp;
            }
        ::close(fd);
    }
    return found;
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
    // The Brightness setting changed (in Settings): the screens follow.
    seen_ = setting();
    connect(Compositor::instance(), &Compositor::settingsChanged, this, [this] { applySetting(); });
    // HDR on or off, or its SDR brightness moved (Settings): what the slider shows.
    connect(Compositor::instance(), &Compositor::outputsChanged, this, [this] {
        hdr_ = hdr();
        emit changed();
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

// Which monitors answer DDC/CI, and where they are, once. Takes a moment
// (a few messages per bus); nothing waits on it. Test boxes leave the real
// monitors alone.
void Brightness::detect() {
    if (!qEnvironmentVariableIsEmpty("ATRIUM_NO_DDC"))
        return;
    QThreadPool::globalInstance()->start([this] {
        Found found = findMonitors();
        QMetaObject::invokeMethod(this, [this, found] {
            buses_ = found.buses;
            if (found.vcp)
                max_ = found.vcp->max;
            emit changed();
            // Monitors don't all keep what DDC set across a power cycle: the
            // setting's, once they answer (again a moment later, for one
            // still waking). Without one, where they are.
            if (const auto v = setting()) {
                setBacklight(*v);
                QTimer::singleShot(1500, this, [this] {
                    if (const auto v = setting())
                        setBacklight(*v);
                });
            } else if (found.vcp) {
                value_ = found.vcp->current * 100 / found.vcp->max;
                emit changed();
            }
        }, Qt::QueuedConnection);
    });
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
