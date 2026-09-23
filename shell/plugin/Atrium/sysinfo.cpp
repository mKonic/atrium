#include "sysinfo.hpp"

#include "sysinfo_core.hpp"
#include "version.hpp"

#include <QDir>
#include <QFile>
#include <QSysInfo>

#include <cmath>
#include <fstream>
#include <pwd.h>
#include <unistd.h>

namespace atrium {

namespace {

QString readText(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// What is installed, as the firmware lists the modules; failing that, what
// the kernel has to use.
QString memoryText() {
    const std::string installed = sysinfo::installed_memory(readText("/run/udev/data/+dmi:id").toStdString());
    if (!installed.empty())
        return QString::fromStdString(installed);
    for (const QString& line : readText("/proc/meminfo").split('\n'))
        if (line.startsWith("MemTotal:")) {
            const double gib = line.section(':', 1).trimmed().section(' ', 0, 0).toDouble() / (1024.0 * 1024.0);
            return QString("%1 GB usable").arg(qRound(gib));
        }
    return {};
}

QStringList gpuNames() {
    struct Gpu {
        unsigned vendor, device;
        unsigned long long vram;  // its largest memory window
    };
    std::vector<Gpu> found;
    for (const QFileInfo& d : QDir("/sys/bus/pci/devices").entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!readText(d.filePath() + "/class").startsWith("0x03"))
            continue;
        found.push_back({readText(d.filePath() + "/vendor").trimmed().toUInt(nullptr, 16),
                         readText(d.filePath() + "/device").trimmed().toUInt(nullptr, 16),
                         sysinfo::largest_bar(readText(d.filePath() + "/resource").toStdString())});
    }
    // Discrete cards map far more memory than integrated graphics: biggest first.
    std::stable_sort(found.begin(), found.end(), [](const Gpu& a, const Gpu& b) { return a.vram > b.vram; });
    QStringList out;
    for (const Gpu& g : found) {
        for (const char* db : {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids"}) {
            std::ifstream ids(db);
            if (!ids)
                continue;
            if (const std::string name = sysinfo::pci_device_name(ids, g.vendor, g.device); !name.empty()) {
                out.push_back(QString::fromStdString(name));
                break;
            }
        }
    }
    return out;
}

} // namespace

SystemInfo::SystemInfo(QObject* parent) : QObject(parent) {
    const std::string osRelease = readText("/etc/os-release").toStdString();
    osName_ = QString::fromStdString(sysinfo::os_release_value(osRelease, "PRETTY_NAME"));
    if (osName_.isEmpty())
        osName_ = QString::fromStdString(sysinfo::os_release_value(osRelease, "NAME"));
    logo_ = QString::fromStdString(sysinfo::os_release_value(osRelease, "LOGO"));
    if (logo_.isEmpty())
        logo_ = QString::fromStdString(sysinfo::os_release_value(osRelease, "ID"));
    hostname_ = QSysInfo::machineHostName();
    kernel_ = QSysInfo::kernelVersion();
    for (const QString& line : readText("/proc/cpuinfo").split('\n'))
        if (line.startsWith("model name")) {
            cpu_ = QString::fromStdString(sysinfo::tidy_cpu_name(line.section(':', 1).toStdString()));
            break;
        }
    gpus_ = gpuNames();
    memory_ = memoryText();
    if (const passwd* pw = getpwuid(getuid())) {
        user_ = QString::fromLocal8Bit(pw->pw_gecos).section(',', 0, 0);
        if (user_.isEmpty())
            user_ = QString::fromLocal8Bit(pw->pw_name);
    }
}

QString SystemInfo::version() const {
    return QStringLiteral(ATRIUM_VERSION);
}

QString SystemInfo::uptime() const {
    const long s = long(readText("/proc/uptime").section(' ', 0, 0).toDouble());
    const long days = s / 86400, hours = s % 86400 / 3600, minutes = s % 3600 / 60;
    const auto part = [](long n, const char* one) { return QString("%1 %2%3").arg(n).arg(one).arg(n == 1 ? "" : "s"); };
    if (days)
        return part(days, "day") + ", " + part(hours, "hour");
    if (hours)
        return part(hours, "hour") + ", " + part(minutes, "minute");
    return part(minutes, "minute");
}

QString SystemInfo::clockTime(double seconds) const {
    return QString::fromStdString(sysinfo::clock_time(seconds));
}

} // namespace atrium
