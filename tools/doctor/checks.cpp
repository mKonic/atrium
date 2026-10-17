#include "checks.hpp"

#include "probe.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace atrium::doctor {

namespace {

constexpr const char* kIssues = "https://github.com/mKonic/atrium/issues";

long long now() {
    return std::time(nullptr);
}

// 18:26 today, else "Oct 15 18:26".
std::string clock(long long when) {
    const std::time_t t = when, n = now();
    std::tm at{}, today{};
    localtime_r(&t, &at);
    localtime_r(&n, &today);
    char buf[32];
    const bool same_day = at.tm_yday == today.tm_yday && at.tm_year == today.tm_year;
    std::strftime(buf, sizeof buf, same_day ? "%H:%M today" : "%b %d %H:%M", &at);
    return buf;
}

std::string plural(long long n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

int parent(int pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(f, line))
        if (line.starts_with("PPid:"))
            return std::atoi(line.c_str() + 5);
    return 0;
}

json ctl(const std::string& command) {
    int status = 0;
    const std::string out = capture({"atriumctl", "-j", command}, &status);
    json j = json::parse(out, nullptr, false);
    return status == 0 && !j.is_discarded() ? j : json();
}

// When `package` was installed (seconds since the epoch); 0 unknown.
long long installed_at(const std::string& package) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/var/lib/pacman/local", ec)) {
        const std::string dir = e.path().filename().string();
        // "atrium-git-0.3.0.r37.g90a9a21-1": the name, then version and release.
        if (!dir.starts_with(package + "-") || std::ranges::count(dir.substr(package.size() + 1), '-') != 1)
            continue;
        std::ifstream desc(e.path() / "desc");
        std::string line;
        while (std::getline(desc, line))
            if (line == "%INSTALLDATE%" && std::getline(desc, line))
                return std::atoll(line.c_str());
    }
    return 0;
}

std::string installed_version(const std::string& package) {
    const std::string q = trim(capture({"pacman", "-Q", package}));
    return q.substr(q.find(' ') + 1);
}

// The compositor's log since it started: read once, several checks look.
const std::vector<std::string>& compositor_log(const Session& s) {
    static std::optional<std::vector<std::string>> log;
    if (!log)
        log = journal("atrium-session", s.started);
    return *log;
}

// A running helper of the session's: the shell's environment is the
// session's (the compositor sets its own after it starts).
std::string session_env(const std::string& key) {
    for (const Process& p : processes("atrium-shell"))
        if (std::string v = environment(p.pid, key); !v.empty())
            return v;
    return {};
}

// --- installed packages ------------------------------------------------------------

bool is_elf(const fs::path& p) {
    char magic[4] = {};
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f)
        return false;
    const bool elf = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "\x7f" "ELF", 4) == 0;
    std::fclose(f);
    return elf;
}

// The installed Qt: where libQt6Core.so.6 points.
QtVersion installed_qt() {
    std::error_code ec;
    const fs::path target = fs::read_symlink("/usr/lib/libQt6Core.so.6", ec);
    return ec ? QtVersion{} : qt_from_soname(target.filename().string());
}

struct Mapped {
    void* data = MAP_FAILED;
    size_t size = 0;
    explicit Mapped(const fs::path& p) {
        const int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
        struct stat st {};
        if (fd < 0)
            return;
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            size = size_t(st.st_size);
            data = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        }
        close(fd);
    }
    ~Mapped() {
        if (data != MAP_FAILED)
            munmap(data, size);
    }
    std::string_view view() const {
        return data == MAP_FAILED ? std::string_view{} : std::string_view(static_cast<const char*>(data), size);
    }
};

Source source_of(const Context& c, const std::string& package) {
    if (package == c.atrium)
        return Source::Atrium;
    return c.foreign.contains(package) ? Source::Aur : Source::Repo;
}

Report install(const Context& c, const Session&) {
    Report r{"install", "Install"};
    const QtVersion qt = installed_qt();
    const std::string qt_text = qt.known() ? "Qt " + std::to_string(qt.major) + "." + std::to_string(qt.minor) : "";
    if (c.atrium.empty()) {
        if (fs::exists("/usr/bin/atrium"))
            r.info("/usr/bin/atrium is from no package (installed by hand): only what it links is checked");
        else
            r.error("atrium isn't installed", "Install atrium-git from the AUR, or a release from " +
                                                  std::string("https://github.com/mKonic/atrium/releases"));
    } else {
        r.summary = c.atrium + " " + installed_version(c.atrium) + (qt_text.empty() ? "" : ", " + qt_text);
        // Files the package should have and doesn't.
        std::vector<std::string> missing;
        for (const std::string& l : lines(capture({"pacman", "-Qkq", c.atrium})))
            missing.push_back(l.substr(l.find(' ') + 1));
        if (!missing.empty()) {
            std::string list;
            for (size_t i = 0; i < missing.size() && i < 5; ++i)
                list += "\n" + missing[i];
            if (missing.size() > 5)
                list += "\nand " + std::to_string(missing.size() - 5) + " more";
            r.error(plural(long(missing.size()), "file of atrium's is missing:", "files of atrium's are missing:") + list,
                    "Reinstall it: atrium-doctor --fix");
        } else {
            r.ok("every file of " + c.atrium + "'s is there");
        }
    }
    const std::vector<Finding> found = examine(c);
    for (const std::string& pkg : packages(found)) {
        std::string text = pkg + " doesn't work with what's installed now:";
        Source source = Source::Repo;
        int shown = 0, more = 0;
        for (const Finding& f : found)
            if ((f.package.empty() ? f.file : f.package) == pkg) {
                source = f.source;
                if (shown++ < 6)
                    text += "\n" + f.file + ": " + f.problem;
                else
                    ++more;
            }
        if (more)
            text += "\nand " + std::to_string(more) + " more";
        r.error(text, remedy(source, false) + "\n(atrium-doctor --fix does that)");
    }
    if (found.empty()) {
        r.ok("every atrium program finds the libraries it was built against");
        r.ok("every Qt plugin was built for the " + (qt_text.empty() ? "Qt" : qt_text) + " installed");
    }
    return r;
}

// --- the session ---------------------------------------------------------------------

Report session_report(const Context& c, const Session& s) {
    Report r{"session", "Session"};
    if (!s.pid) {
        r.skip(s.why);
        return r;
    }
    r.summary = s.version + ", up " + duration(now() - s.started);
    // The build running against the one installed.
    const std::string running = build_commit(s.version);
    const std::string installed = c.atrium.empty() ? "" : build_commit(installed_version(c.atrium));
    const bool outdated = !running.empty() && !installed.empty() && running != installed;
    if (outdated)
        r.warn("The desktop running (" + s.version + ") isn't the build installed (" +
                   installed_version(c.atrium) + "), nor are its helpers",
               "Log out and back in to start the installed one.");
    else
        r.ok("running the build installed");

    struct Helper {
        const char* name;
        const char* does;
        Level missing;
    };
    for (const Helper& h : {Helper{"atrium-shell", "the menu bar, Dock and desktop", Level::Error},
                            Helper{"atrium-keyring", "saved passwords", Level::Warn},
                            Helper{"atrium-clipsync", "the clipboard shared with a phone", Level::Warn},
                            Helper{"atrium-phonelink", "a phone's sound", Level::Warn}}) {
        const std::vector<Process> ps = processes(h.name);
        if (ps.empty()) {
            const std::string text = std::string(h.name) + " (" + h.does + ") isn't running";
            if (h.missing == Level::Error)
                r.error(text, "atrium starts it again on its own; if it doesn't come back, see Log and Crashes.");
            else
                r.warn(text, "Log out and back in.");
            continue;
        }
        const Process& p = ps.front();
        if (p.stale && outdated)
            r.info(std::string(h.name) + " (pid " + std::to_string(p.pid) + ") runs the build from before the update");
        else if (p.stale)
            r.warn(std::string(h.name) + " (pid " + std::to_string(p.pid) +
                       ") runs the build from before the last update",
                   "Log out and back in.");
        else
            r.ok(std::string(h.name) + " is running (pid " + std::to_string(p.pid) + ")");
    }

    // The shell, started again after it ended; the compositor, by its
    // crash-recovery wrapper.
    std::vector<std::string> restarts;
    bool safe_mode = false;
    for (const std::string& l : compositor_log(s)) {
        if (l.find("shell: exited with") != std::string::npos || l.find("shell: killed by signal") != std::string::npos)
            restarts.push_back(l.substr(l.find("shell: ") + 7));
        safe_mode |= l.find("shell: keeps failing") != std::string::npos;
    }
    if (safe_mode)
        r.error("The shell kept failing to start this session, so it ran in safe mode (no effects)",
                "atrium-doctor install and Crashes say why, if they know; else please report it: " +
                    std::string(kIssues));
    else if (!restarts.empty())
        r.warn("The shell ended " + plural(long(restarts.size()), "time", "times") +
                   " this session and was started again; the last time it " + restarts.back(),
               "Its crash, if it was one, is under Crashes; its log: journalctl --user -t atrium-shell");
    else
        r.ok("the shell has run since the session started");

    if (const int wrapper = parent(s.pid); wrapper > 0 && process(wrapper).exe == process(s.pid).exe) {
        int crashes = 0;
        for (const std::string& l : journal("atrium-session", now() - age(wrapper)))
            crashes += l.find("wrapper: the compositor crashed") != std::string::npos;
        if (crashes)
            r.error("The compositor crashed " + plural(crashes, "time", "times") +
                        " this session (the windows came back each time)",
                    "Its crash is under Crashes. Please report it: " + std::string(kIssues));
        else
            r.ok("the compositor hasn't crashed this session");
    }
    return r;
}

// --- crashes ---------------------------------------------------------------------------

Report crashes(const Context& c, const Session&) {
    Report r{"crashes", "Crashes"};
    const json dumps = json::parse(capture({"coredumpctl", "--json=short", "--since=-7d", "--no-pager"}), nullptr, false);
    const long long installed = c.atrium.empty() ? 0 : installed_at(c.atrium);
    struct Group {
        int count = 0, since_install = 0;
        json last;
    };
    std::map<std::string, Group> by;
    if (dumps.is_array())
        for (const json& d : dumps) {
            const std::string exe = d.value("exe", "");
            const std::string name = exe.substr(exe.rfind('/') + 1);
            if (!name.starts_with("atrium") && name != "Xwayland")
                continue;
            Group& g = by[name];
            ++g.count;
            g.since_install += d.value("time", 0LL) / 1000000 >= installed;
            if (g.last.is_null() || d.value("time", 0LL) > g.last.value("time", 0LL))
                g.last = d;
        }
    int recent = 0;
    for (const auto& [name, g] : by) {
        const long long when = g.last.value("time", 0LL) / 1000000;
        const int pid = g.last.value("pid", 0);
        const char* sig = sigabbrev_np(g.last.value("sig", 0));
        std::string how = sig ? "SIG" + std::string(sig) : "signal " + std::to_string(g.last.value("sig", 0));
        if (g.since_install == 0) {
            r.info(name + " crashed " + plural(g.count, "time", "times") +
                   " this week, all before this build was installed (the last at " + clock(when) + ")");
            continue;
        }
        const std::string info = capture({"coredumpctl", "info", "--no-pager", std::to_string(pid)});
        if (const std::string frame = top_frame(info); !frame.empty())
            how += ", in " + frame;
        // Started before the update, it crashed running the build before.
        if (info.find(g.last.value("exe", "") + " (deleted)") != std::string::npos) {
            r.info(name + " crashed at " + clock(when) + " (" + how + "), running the build from before the update");
            continue;
        }
        recent += g.since_install;
        r.error(name + " crashed " + (g.since_install == 1 ? std::string("once") : plural(g.since_install, "time", "times")) +
                    " since this build was installed, the last at " + clock(when) + " (" + how + ")",
                "coredumpctl info " + std::to_string(pid) + " shows the whole trace.\nPlease report it: " + kIssues);
    }
    if (by.empty())
        r.ok("no atrium program or Xwayland crashed this week");
    r.summary = recent ? plural(recent, "crash", "crashes") + " since this build was installed"
                       : "none since this build was installed";
    return r;
}

// --- the logs ----------------------------------------------------------------------

Report log(const Context&, const Session& s) {
    Report r{"log", "Log"};
    if (!s.pid) {
        r.skip(s.why);
        return r;
    }
    // The compositor's own errors (apps it started log there too: only its
    // "[ERROR]" lines are its own).
    std::vector<std::string> errors;
    for (const std::string& l : compositor_log(s))
        if (const size_t at = l.find("[ERROR] "); at != std::string::npos)
            errors.push_back(l.substr(at + 8));
    const std::vector<Tally> told = tally(errors);
    r.info(plural(long(errors.size()), "error", "errors") + " logged by the compositor this session" +
           (errors.empty() ? "" : ":"));
    for (size_t i = 0; i < told.size() && i < 8; ++i)
        r.info("  " + (told[i].count > 1 ? std::to_string(told[i].count) + "× " : "") + told[i].example);

    // The shell's QML errors are bugs; its other warnings are Qt's chatter.
    std::vector<std::string> qml, other;
    for (const std::string& l : journal("atrium-shell", s.started, "warning"))
        (qml_error(l) ? qml : other).push_back(l);
    for (const Tally& t : tally(qml))
        r.warn("The shell: " + t.example + (t.count > 1 ? " (" + std::to_string(t.count) + "×)" : ""),
               "A bug in atrium's shell. Please report it: " + std::string(kIssues));
    if (qml.empty())
        r.ok("no QML errors from the shell this session");
    r.info(plural(long(other.size()), "other warning", "other warnings") + " from the shell this session");
    r.summary = qml.empty() ? plural(long(errors.size()), "compositor error", "compositor errors") +
                                  ", no shell errors"
                            : plural(long(qml.size()), "shell error", "shell errors");
    return r;
}

// --- portals -------------------------------------------------------------------------

Report portals(const Context&, const Session& s) {
    Report r{"portals", "Portals"};
    if (!s.pid) {
        r.skip(s.why);
        return r;
    }
    int status = 0;
    const std::string scheme = capture({"busctl", "--user", "--timeout=5", "call", "org.freedesktop.portal.Desktop",
                                        "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Settings",
                                        "ReadOne", "ss", "org.freedesktop.appearance", "color-scheme"},
                                       &status, true);
    if (status == 0) {
        r.ok("xdg-desktop-portal answers");
    } else {
        r.error("xdg-desktop-portal doesn't answer: apps can't open or save files, share the screen or follow "
                "light and dark",
                "systemctl --user restart xdg-desktop-portal");
    }

    // Its failures since it started: those of a backend since replaced are gone with it.
    const int backend = bus_owner("org.freedesktop.impl.portal.desktop.atrium");
    const long long backend_since = backend ? std::max(s.started, now() - age(backend)) : s.started;
    if (const int pid = backend; pid == 0) {
        r.info("atrium-portal isn't running yet: D-Bus starts it when an app first asks");
    } else if (const Process p = process(pid); p.stale) {
        r.warn("atrium-portal (pid " + std::to_string(pid) + ") runs the build from before the last update",
               "kill " + std::to_string(pid) + "  (D-Bus starts the new one when an app next asks)");
    } else {
        r.ok("atrium-portal is the portal's backend (pid " + std::to_string(pid) + ")");
    }

    const std::string desktop = session_env("XDG_CURRENT_DESKTOP");
    if (!desktop.empty() && desktop != "atrium")
        r.warn("The session says it's " + desktop + ", not atrium: the portal picks that desktop's backends",
               "Something sets XDG_CURRENT_DESKTOP; it should be atrium.");
    // The user's own choice of backends, over atrium's.
    const char* home = std::getenv("HOME");
    const std::string config = std::getenv("XDG_CONFIG_HOME") ? std::getenv("XDG_CONFIG_HOME")
                                                               : std::string(home ? home : "") + "/.config";
    for (const std::string& file : {config + "/xdg-desktop-portal/atrium-portals.conf",
                                    config + "/xdg-desktop-portal/portals.conf"}) {
        std::ifstream f(file);
        std::string line;
        while (std::getline(f, line))
            if (line.starts_with("default=") && !line.substr(8).starts_with("atrium"))
                r.warn(file + " picks " + line.substr(8) + " over atrium's portal",
                       "Delete it, or make it say default=atrium.");
        if (f.is_open())
            break;  // the first found is the one used
    }

    std::ifstream mounts("/proc/mounts");
    const std::string all((std::istreambuf_iterator<char>(mounts)), {});
    if (all.find(" /run/user/" + std::to_string(getuid()) + "/doc ") != std::string::npos)
        r.ok("the document portal is up");
    else
        r.warn("The document portal isn't running: files dragged in from a file manager (onto the Dock too) "
               "and sandboxed apps' files don't come through",
               "systemctl --user restart xdg-document-portal");

    // What the portal says went wrong with atrium's backend.
    std::vector<std::string> failed;
    for (const std::string& l : journal("/usr/lib/xdg-desktop-portal", backend_since, "warning"))
        if (l.find("backend call failed") != std::string::npos || l.find("Backend call failed") != std::string::npos)
            failed.push_back(l);
    for (const Tally& t : tally(failed))
        r.warn("xdg-desktop-portal: " + t.example + (t.count > 1 ? " (" + std::to_string(t.count) + "×)" : ""),
               "A bug in atrium-portal. Please report it: " + std::string(kIssues));
    r.summary = status == 0 ? "xdg-desktop-portal answers" : "xdg-desktop-portal doesn't answer";
    return r;
}

// --- D-Bus services and user units ----------------------------------------------------

Report services(const Context&, const Session& s) {
    Report r{"services", "Services"};
    if (!s.pid) {
        r.skip(s.why);
        return r;
    }
    struct Service {
        const char* name;
        const char* by;
        const char* does;
        Level missing;
    };
    int theirs = 0;
    for (const Service& sv :
         {Service{"org.freedesktop.Notifications", "atrium-shell", "notifications", Level::Warn},
          Service{"org.kde.StatusNotifierWatcher", "atrium-shell", "apps' icons in the menu bar", Level::Warn},
          Service{"com.canonical.Unity", "atrium-shell", "the Dock's badges and progress bars", Level::Info},
          Service{"org.freedesktop.secrets", "atrium-keyring", "saved passwords", Level::Warn},
          Service{"org.atrium.ClipSync", "atrium-clipsync", "the clipboard shared with a phone", Level::Info},
          Service{"org.atrium.PhoneLink", "atrium-phonelink", "a phone's sound", Level::Info}}) {
        const int pid = bus_owner(sv.name);
        const Process p = pid ? process(pid) : Process{};
        const std::string exe = p.exe.substr(p.exe.rfind('/') + 1);
        if (!pid) {
            const std::string text = std::string("Nothing answers for ") + sv.does + " (" + sv.name + ")";
            if (sv.missing == Level::Warn)
                r.warn(text, std::string(sv.by) + " should: see Session.");
            else
                r.info(text);
        } else if (exe != sv.by) {
            ++theirs;
            const std::string u = unit(pid);
            r.warn(exe + " takes " + sv.does + " (" + sv.name + ") from atrium",
                   u.ends_with(".service") ? "systemctl --user disable --now " + u
                                           : "Stop " + exe + " starting with your session.");
        } else {
            r.ok(std::string(sv.does) + ": " + sv.by);
        }
    }

    // User services that failed: this session's matter.
    int failed = 0;
    for (const std::string& l : lines(capture({"systemctl", "--user", "--failed", "--plain", "--no-legend"}))) {
        const std::string u = l.substr(0, l.find(' '));
        const std::string when =
            trim(capture({"systemctl", "--user", "show", "-p", "InactiveEnterTimestamp", "--value",
                          "--timestamp=unix", u}));
        const long long at = when.starts_with("@") ? std::atoll(when.c_str() + 1) : 0;
        if (at >= s.started) {
            ++failed;
            r.warn(u + " failed this session", "journalctl --user -u " + u + " -b shows why.");
        } else {
            r.info(u + " failed before this session (" + clock(at) + ")");
        }
    }
    if (!failed)
        r.ok("no user service failed this session");
    r.summary = theirs ? plural(theirs, "service", "services") + " taken by another program"
                       : "atrium's own answer";
    return r;
}

// --- displays and the GPU -------------------------------------------------------------

std::string hz(double refresh) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.0f Hz", refresh);
    return buf;
}

Report display(const Context&, const Session& s) {
    Report r{"display", "Display"};
    std::vector<std::string> drivers;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/class/drm", ec)) {
        const std::string card = e.path().filename().string();
        if (!card.starts_with("card") || card.find('-') != std::string::npos)
            continue;
        const std::string driver = fs::read_symlink(e.path() / "device/driver", ec).filename().string();
        if (!ec && std::ranges::find(drivers, driver) == drivers.end())
            drivers.push_back(driver);
    }
    for (const std::string& d : drivers) {
        std::string version;
        std::ifstream(d == "nvidia" ? "/sys/module/nvidia/version" : "") >> version;
        r.info("GPU driver: " + d + (version.empty() ? "" : " " + version));
    }
    if (std::ranges::find(drivers, "nvidia") != drivers.end()) {
        std::string modeset;
        std::ifstream("/sys/module/nvidia_drm/parameters/modeset") >> modeset;
        if (modeset != "Y")
            r.error("NVIDIA's kernel modesetting is off: atrium can't drive the screens",
                    "Add nvidia_drm.modeset=1 to the kernel's command line and reboot.");
        else
            r.ok("NVIDIA's kernel modesetting is on");
    }
    if (!fs::exists("/usr/bin/Xwayland"))
        r.warn("Xwayland isn't installed: X11 apps can't open", "sudo pacman -S xorg-xwayland");
    if (!s.pid) {
        r.summary = drivers.empty() ? "" : drivers.front();
        return r;
    }
    const json outputs = ctl("outputs");
    std::string summary;
    if (outputs.is_array())
        for (const json& o : outputs) {
            if (!o.value("enabled", true))
                continue;
            const json mode = o.value("mode", json::object());
            const int w = mode.value("width", 0), h = mode.value("height", 0);
            const double refresh = o.value("refresh", 0.0);
            const std::string name = o.value("name", "");
            std::string text = name + " (" + o.value("make", "") + " " + o.value("model", "") + "): " +
                               std::to_string(w) + "×" + std::to_string(h) + " at " + hz(refresh);
            if (const double scale = o.value("scale", 1.0); scale != 1.0)
                text += ", scale " + std::to_string(scale).substr(0, 4);
            if (o.value("hdr_active", false))
                text += ", HDR";
            if (const std::string vrr = o.value("adaptive_sync", "off"); vrr != "off")
                text += ", VRR " + (vrr == "on" ? std::string("always") : "in " + vrr);
            r.info(text);
            summary += (summary.empty() ? "" : ", ") + name + " " + std::to_string(w) + "×" + std::to_string(h) +
                       " " + hz(refresh);
            // The fastest it goes at this size.
            double best = 0;
            for (const json& m : o.value("modes", json::array()))
                if (m.value("width", 0) == w && m.value("height", 0) == h)
                    best = std::max(best, m.value("refresh", 0) / 1000.0);
            if (best > refresh + 1)
                r.warn(name + " runs at " + hz(refresh) + "; it can do " + hz(best) + " at this size",
                       "Settings > Displays, or: atriumctl output " + name +
                           " refresh=" + std::to_string(int(std::lround(best))));
        }
    r.summary = summary;
    const std::vector<Process> x = processes("Xwayland");
    r.info(x.empty() ? "Xwayland starts when an X11 app opens"
                     : "Xwayland is running (pid " + std::to_string(x.front().pid) + ")");
    return r;
}

// --- sound -------------------------------------------------------------------------------

Report sound(const Context&, const Session&) {
    Report r{"sound", "Sound"};
    for (const char* u : {"pipewire.service", "wireplumber.service", "pipewire-pulse.socket"}) {
        const std::string state = trim(capture({"systemctl", "--user", "is-active", u}));
        if (state == "active")
            r.ok(std::string(u) + " is active");
        else
            r.error(std::string(u) + " is " + (state.empty() ? "unknown" : state) + ": no sound",
                    std::string("systemctl --user restart ") + u);
    }
    const std::string sink = trim(capture({"pactl", "get-default-sink"}));
    std::string described = sink;
    const json sinks = json::parse(capture({"pactl", "-f", "json", "list", "sinks"}), nullptr, false);
    if (sinks.is_array())
        for (const json& k : sinks)
            if (k.value("name", "") == sink)
                described = k.value("description", sink);
    if (sink.empty() || sink == "auto_null")
        r.warn("Nothing to play sound to: no speakers or headphones", "Plug some in, or pick them in Settings > Sound.");
    else
        r.ok("plays to " + described);
    r.summary = sink.empty() || sink == "auto_null" ? "nothing to play to" : described;
    return r;
}

// --- keyboards and input methods -----------------------------------------------------------

Report input(const Context&, const Session& s) {
    Report r{"input", "Input"};
    if (!s.pid) {
        r.skip(s.why);
        return r;
    }
    // Xwayland compiles each keymap it's sent: often means something keeps
    // sending one (a keyboard switched back and forth).
    long long compiles = 0;
    for (const std::string& l : compositor_log(s))
        compiles += l.starts_with("The XKEYBOARD keymap compiler");
    const double hours = std::max(0.25, double(now() - s.started) / 3600);
    if (compiles > 20 && compiles / hours > 30)
        r.warn("Xwayland was sent " + plural(compiles, "new keymap", "new keymaps") +
                   " this session (and every app each one)",
               "Something keeps switching the keyboard (an input method's, a remapping tool's). Please report it "
               "with what you were typing in: " +
                   std::string(kIssues));
    else
        r.ok("keymaps are steady (" + plural(compiles, "compile", "compiles") + " by Xwayland this session)");

    for (const char* ime : {"fcitx5", "ibus-daemon"})
        if (!processes(ime).empty())
            r.info(std::string("input method: ") + ime);
    const json devices = ctl("devices");
    std::string names;
    if (devices.is_array())
        for (const json& d : devices)
            names += (names.empty() ? "" : ", ") + d.value("name", "");
    if (!names.empty())
        r.info("devices: " + names);
    r.summary = compiles > 20 && compiles / hours > 30 ? "keymap resent too often" : "";
    return r;
}

// --- the system ---------------------------------------------------------------------------

Report system(const Context&, const Session& s) {
    Report r{"system", "System"};
    std::ifstream release("/etc/os-release");
    std::string line, distro;
    while (std::getline(release, line))
        if (line.starts_with("PRETTY_NAME="))
            distro = line.substr(13, line.size() - 14);
    const std::string kernel = trim(capture({"uname", "-r"}));
    r.summary = distro + ", " + kernel;
    for (const char* dm : {"atrium-login", "sddm", "gdm", "lightdm", "greetd", "ly"})
        if (trim(capture({"systemctl", "is-enabled", std::string(dm) + ".service"})) == "enabled")
            r.info(std::string("logs in with ") + dm);
    if (s.pid) {
        if (const std::string type = session_env("XDG_SESSION_TYPE"); !type.empty() && type != "wayland")
            r.warn("The session says it's " + type + ", not wayland: apps may pick X11",
                   "Something sets XDG_SESSION_TYPE; it should be wayland.");
        if (const std::string qpa = session_env("QT_QPA_PLATFORM"); qpa.starts_with("xcb"))
            r.warn("QT_QPA_PLATFORM=" + qpa + ": Qt apps run under Xwayland, blurry when scaled",
                   "Remove it from your environment (it's set in your profile or environment.d).");
    }
    struct statvfs v {};
    const char* home = std::getenv("HOME");
    if (home && statvfs(home, &v) == 0) {
        const double free_gib = double(v.f_bavail) * double(v.f_frsize) / (1024.0 * 1024 * 1024);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.1f GiB free in your home", free_gib);
        if (free_gib < 0.2)
            r.error(buf + std::string(": settings and the clipboard history can't be saved"),
                    "Free some space.");
        else if (free_gib < 1)
            r.warn(buf, "Free some space: atrium saves settings and the clipboard history there.");
        else
            r.ok(buf);
    }
    return r;
}

} // namespace

Context context() {
    Context c;
    if (fs::exists("/usr/bin/atrium"))
        c.atrium = owner("/usr/bin/atrium");
    for (const std::string& p : lines(capture({"pacman", "-Qqm"})))
        c.foreign.insert(p);
    return c;
}

std::vector<Finding> examine(const Context& c) {
    std::vector<Finding> found;
    auto add = [&](const std::string& file, const std::string& problem, std::string package = {}) {
        if (package.empty())
            package = owner(file);
        found.push_back({package, source_of(c, package), file, problem});
    };

    // atrium's own binaries and libraries: everything they link still there.
    if (!c.atrium.empty()) {
        std::vector<std::string> elves;
        for (const std::string& f : lines(capture({"pacman", "-Qlq", c.atrium})))
            if (fs::is_regular_file(f) && is_elf(f))
                elves.push_back(f);
        std::vector<std::vector<std::string>> problems(elves.size());
        std::vector<std::thread> workers;
        const size_t n = std::max(1u, std::min(4u, std::thread::hardware_concurrency()));
        for (size_t w = 0; w < n; ++w)
            workers.emplace_back([&, w] {
                for (size_t i = w; i < elves.size(); i += n)
                    problems[i] = ldd_problems(capture({"ldd", elves[i]}, nullptr, true));
            });
        for (std::thread& t : workers)
            t.join();
        for (size_t i = 0; i < elves.size(); ++i)
            for (const std::string& p : problems[i])
                add(elves[i], p, c.atrium);
    }

    // Every Qt plugin, atrium's and the system's: built for the Qt installed.
    struct Plugin {
        std::string file, dir;
        QtVersion built;
        bool private_api;
    };
    const QtVersion qt = installed_qt();
    std::vector<Plugin> other;  // built for another Qt
    for (const char* root : {"/usr/lib/qt6/plugins", "/usr/lib/atrium"}) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (!it->is_regular_file() || it->path().extension() != ".so")
                continue;
            const Mapped m(it->path());
            const QtVersion built = plugin_qt(m.view());
            if (built.known() && !(built == qt))
                other.push_back({it->path().string(), it->path().parent_path().filename().string(), built,
                                 uses_private_qt(m.view())});
        }
    }
    if (other.empty())
        return found;
    std::vector<std::string> query = {"pacman", "-Qo"};
    for (const Plugin& p : other)
        query.push_back(p.file);
    const auto owners = parse_owners(capture(query));
    for (const Plugin& p : other) {
        std::string package;
        for (const auto& [file, pkg] : owners)
            if (file == p.file)
                package = pkg;
        const std::string problem = plugin_problem(p.built, qt, p.dir, p.private_api, source_of(c, package));
        if (!problem.empty())
            found.push_back({package, source_of(c, package), p.file, problem});
    }
    return found;
}

Session session() {
    Session s;
    const json v = ctl("version");
    if (!v.is_object()) {
        s.why = "not in an atrium session";
        return s;
    }
    s.version = v.value("version", "");
    // Under the crash-recovery wrapper there are two: the wrapper's child is
    // the compositor.
    std::vector<Process> ps = processes("atrium");
    if (ps.empty())
        for (const std::string& pid : lines(capture({"pgrep", "-u", std::to_string(getuid()), "-x", "atrium"})))
            ps.push_back(process(std::atoi(pid.c_str())));
    for (const Process& p : ps)
        if (p.pid && (!s.pid || std::ranges::any_of(ps, [&](const Process& q) { return q.pid == parent(p.pid); })))
            s.pid = p.pid;
    if (!s.pid) {
        s.why = "atrium answers, but its process can't be found";
        return s;
    }
    s.started = now() - age(s.pid);
    return s;
}

const std::vector<Check>& checks() {
    static const std::vector<Check> all = {
        {"install", "atrium's files, and the libraries and Qt plugins it runs on", install},
        {"session", "the running build, its helpers, shell restarts", session_report},
        {"crashes", "crashes of atrium's programs and Xwayland this week", crashes},
        {"log", "errors logged this session", log},
        {"portals", "xdg-desktop-portal, atrium's backend, the document portal", portals},
        {"services", "who answers for notifications, the tray, passwords; failed user services", services},
        {"display", "the GPU, the screens and their modes", display},
        {"sound", "PipeWire and what it plays to", sound},
        {"input", "keymaps, input methods, pointing devices", input},
        {"system", "the distribution, login, the session's environment, free space", system},
    };
    return all;
}

} // namespace atrium::doctor
