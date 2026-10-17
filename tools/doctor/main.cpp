// atrium-doctor: a health check of atrium and the session it runs, section
// by section (checks.hpp), as flutter doctor and Neovim's :checkhealth tell
// theirs; and fixing what's installed when what atrium (and the Qt plugins
// around it) was built against has moved: a newer release, else the latest
// atrium built from source; AUR packages rebuilt from the AUR. Links nothing
// but the C++ runtime, so it still runs when what atrium uses has moved.
//
//   atrium-doctor [SECTION...] [-v] [--json]   the health check
//   atrium-doctor --list   its sections
//   atrium-doctor --fix    fix what's installed (asks first)
//   atrium-doctor --brief  one line when what's installed is wrong (pacman's hook)
//   atrium-doctor --notify a notification when it is (login)

#include "checks.hpp"
#include "doctor_core.hpp"
#include "probe.hpp"
#include "updates_core.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace atrium::doctor;

namespace {

constexpr const char* kRepo = "https://github.com/mKonic/atrium.git";
constexpr const char* kLatest = "https://api.github.com/repos/mKonic/atrium/releases/latest";

struct Release {
    std::string tag, url, sha256;
};

// atrium's latest release, when GitHub answers.
Release latest_release() {
    int status = 0;
    const std::string body = capture({"curl", "-fsSL", "--max-time", "15", "-H", "Accept: application/vnd.github+json", kLatest}, &status);
    Release r;
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (status != 0 || !j.is_object())
        return r;
    r.tag = j.value("tag_name", "");
    for (const auto& a : j.value("assets", nlohmann::json::array())) {
        const std::string name = a.value("name", "");
        if (name.ends_with(".pkg.tar.zst")) {
            r.url = a.value("browser_download_url", "");
            const std::string digest = a.value("digest", "");
            if (digest.starts_with("sha256:"))
                r.sha256 = digest.substr(7);
        }
    }
    return r;
}

// Whether the release is newer than the installed package.
bool newer_than_installed(const Release& r, const std::string& package) {
    if (r.tag.empty() || r.url.empty() || package.empty())
        return false;
    // "atrium-git 0.2.0.r24.g609dbba-1" → "0.2.0.r24.g609dbba"
    std::string installed = trim(capture({"pacman", "-Q", package}));
    installed = installed.substr(installed.find(' ') + 1);
    installed = installed.substr(0, installed.rfind('-'));
    const std::string out = trim(capture({"vercmp", atrium::updates::pkgver(r.tag, 0), installed}));
    return !out.empty() && std::atoi(out.c_str()) > 0;
}

bool ask(const char* question) {
    std::printf("\n%s [Y/n] ", question);
    std::fflush(stdout);
    std::string answer;
    if (!std::getline(std::cin, answer))
        return false;
    return answer.empty() || answer[0] == 'y' || answer[0] == 'Y';
}

std::string temp_dir() {
    std::string base = std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp";
    std::string tmpl = base + "/atrium-doctor-XXXXXX";
    return mkdtemp(tmpl.data()) ? tmpl : std::string{};
}

int jobs() {
    return int(std::max(1u, std::thread::hardware_concurrency() / 2));
}

bool install_release(const Release& r, const std::string& dir) {
    const std::string file = dir + "/" + r.url.substr(r.url.rfind('/') + 1);
    if (!run({"curl", "-fL", "--progress-bar", "-o", file, r.url}))
        return false;
    if (!r.sha256.empty()) {
        const std::string sum = capture({"sha256sum", file});
        if (sum.substr(0, sum.find(' ')) != r.sha256) {
            std::printf("The download doesn't match its checksum; nothing was installed.\n");
            return false;
        }
    }
    return run({"sudo", "pacman", "-U", "--noconfirm", file});
}

bool build_atrium(const std::string& dir) {
    const std::string src = dir + "/atrium";
    if (!run({"git", "clone", kRepo, src}))
        return false;
    const std::string pkgdir = src + "/packaging/arch";
    if (!run({"makepkg", "-sf", "--nocheck", "--noconfirm"}, pkgdir, {"MAKEFLAGS=-j" + std::to_string(jobs())}))
        return false;
    std::vector<std::string> install = {"sudo", "pacman", "-U", "--noconfirm"};
    for (const auto& e : fs::directory_iterator(pkgdir))
        if (e.path().string().ends_with(".pkg.tar.zst") && e.path().filename().string().find("-debug-") == std::string::npos)
            install.push_back(e.path().string());
    return install.size() > 4 && run(install);
}

bool rebuild_aur(const std::string& package, const std::string& dir) {
    const std::string src = dir + "/" + package;
    if (!run({"git", "clone", "https://aur.archlinux.org/" + package + ".git", src}) ||
        !fs::exists(src + "/PKGBUILD")) {
        std::printf("%s isn't on the AUR: rebuild it the way you installed it.\n", package.c_str());
        return false;
    }
    return run({"makepkg", "-sif", "--noconfirm"}, src, {"MAKEFLAGS=-j" + std::to_string(jobs())});
}

void report(const std::vector<Finding>& found, bool newer) {
    if (found.empty()) {
        std::printf("Everything atrium uses works with what's installed.\n");
        return;
    }
    for (const std::string& pkg : packages(found)) {
        Source source = Source::Repo;
        std::printf("\n%s\n", pkg.c_str());
        for (const Finding& f : found)
            if ((f.package.empty() ? f.file : f.package) == pkg) {
                std::printf("  %s: %s\n", f.file.c_str(), f.problem.c_str());
                source = f.source;
            }
        std::printf("  fix: %s\n", remedy(source, newer).c_str());
    }
}

int fix(const std::vector<Finding>& found, const Context& c, const Release& release, bool newer) {
    if (found.empty())
        return 0;
    if (geteuid() == 0) {
        std::printf("\nRun atrium-doctor --fix as yourself, not root: it asks for your password when it needs it.\n");
        return 1;
    }
    if (!ask("Fix these now?"))
        return 1;
    const std::string dir = temp_dir();
    if (dir.empty())
        return 1;
    bool ok = true;
    std::set<Source> sources;
    std::vector<std::string> aur;
    for (const Finding& f : found) {
        sources.insert(f.source);
        if (f.source == Source::Aur && std::ranges::find(aur, f.package) == aur.end())
            aur.push_back(f.package);
    }
    // The system first (it brings the Qt the rest is rebuilt against), then
    // the AUR, then atrium.
    if (sources.contains(Source::Repo))
        ok &= run({"sudo", "pacman", "-Syu"});
    for (const std::string& pkg : aur)
        ok &= rebuild_aur(pkg, dir);
    if (sources.contains(Source::Atrium))
        ok &= newer ? install_release(release, dir) : build_atrium(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (!ok) {
        std::printf("\nNot everything was fixed; see above.\n");
        return 1;
    }
    const std::vector<Finding> after = examine(c);
    if (!after.empty()) {
        std::printf("\nStill not right:\n");
        report(after, false);
        return 1;
    }
    std::printf("\nFixed. Log out and back in to start the new atrium.\n");
    return 0;
}

// Once the shell's notification server answers (at login it may still be
// starting): a minute at most.
void notify(const std::string& body) {
    for (int i = 0; i < 60; ++i) {
        int status = 0;
        capture({"gdbus", "call", "--session", "--dest", "org.freedesktop.Notifications", "--object-path",
             "/org/freedesktop/Notifications", "--method", "org.freedesktop.Notifications.Notify", "atrium", "0",
                 "dialog-warning", "Something atrium uses was updated", body, "[]", "{}", "0"},
                &status);
        if (status == 0)
            return;
        sleep(1);
    }
}

// The health check: each section, or those named.
int health(const std::vector<std::string>& names, bool verbose, bool as_json) {
    const Context c = context();
    const Session s = session();
    const bool tty = isatty(STDERR_FILENO);
    std::vector<Report> reports;
    for (const Check& check : checks()) {
        if (!names.empty() && std::ranges::find(names, check.name) == names.end())
            continue;
        if (tty && !as_json)
            std::fprintf(stderr, "\r\033[Kchecking %s…", check.name);
        reports.push_back(check.run(c, s));
    }
    if (tty && !as_json)
        std::fprintf(stderr, "\r\033[K");
    bool wrong = false;
    for (const Report& r : reports)
        wrong |= r.status() == Level::Warn || r.status() == Level::Error;
    if (as_json) {
        auto level = [](Level l) {
            switch (l) {
            case Level::Ok: return "ok";
            case Level::Info: return "info";
            case Level::Skip: return "skip";
            case Level::Warn: return "warn";
            case Level::Error: return "error";
            }
            return "";
        };
        nlohmann::json out = {{"version", s.version}, {"sections", nlohmann::json::array()}};
        for (const Report& r : reports) {
            nlohmann::json items = nlohmann::json::array();
            for (const Item& i : r.items)
                items.push_back({{"level", level(i.level)}, {"text", i.text}, {"advice", i.advice}});
            out["sections"].push_back({{"name", r.name}, {"title", r.title}, {"status", level(r.status())},
                                       {"summary", r.summary}, {"items", items}});
        }
        std::printf("%s\n", out.dump(2).c_str());
    } else {
        const bool color = isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR");
        std::fputs(render(reports, verbose, color).c_str(), stdout);
    }
    return wrong ? 1 : 0;
}

int usage(bool ok) {
    std::fprintf(ok ? stdout : stderr,
                 "usage: atrium-doctor [SECTION...] [-v] [--json]\n"
                 "       atrium-doctor --list | --fix | --brief | --notify\n"
                 "  (none)    check atrium and its session, each section (or those named)\n"
                 "  -v        everything looked at, not just what's wrong\n"
                 "  --json    the same, for scripts\n"
                 "  --list    the sections\n"
                 "  --fix     fix what's installed when what atrium uses moved under it: a newer\n"
                 "            atrium release, else atrium built from source; AUR packages rebuilt;\n"
                 "            a system update for the rest\n"
                 "  --brief   one line when what's installed is wrong, nothing otherwise\n"
                 "  --notify  a desktop notification when it is\n"
                 "Exits 1 when something's wrong.\n");
    return ok ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode;
    bool shell_failed = false, verbose = false, as_json = false;
    std::vector<std::string> names;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--fix" || a == "--brief" || a == "--notify") {
            mode = a;
        } else if (a == "--shell-failed") {
            // From atrium, in a terminal, when the shell keeps failing.
            mode = "--fix";
            shell_failed = true;
        } else if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--json") {
            as_json = true;
        } else if (a == "--list") {
            for (const Check& c : checks())
                std::printf("%-9s %s\n", c.name, c.about);
            return 0;
        } else if (a == "-h" || a == "--help") {
            return usage(true);
        } else if (!a.starts_with("-") &&
                   std::ranges::any_of(checks(), [&](const Check& c) { return a == c.name; })) {
            names.push_back(a);
        } else {
            if (!a.starts_with("-"))
                std::fprintf(stderr, "atrium-doctor: no section \"%s\" (atrium-doctor --list)\n", a.c_str());
            return usage(false);
        }
    }
    if (mode.empty())
        return health(names, verbose, as_json);

    const Context c = context();
    const std::vector<Finding> found = examine(c);
    if (mode == "--brief" || mode == "--notify") {
        const std::string line = brief(found);
        if (!line.empty()) {
            if (mode == "--notify")
                notify(line);
            else
                std::printf("%s\n", line.c_str());
        }
        return 0;  // pacman's hook: a finding isn't a failed hook
    }

    if (shell_failed)
        std::printf("atrium's desktop shell keeps failing to start. Checking why...\n");
    const bool atrium_broken = std::ranges::any_of(found, [](const Finding& f) { return f.source == Source::Atrium; });
    const Release release = atrium_broken ? latest_release() : Release{};
    const bool newer = newer_than_installed(release, c.atrium);
    report(found, newer);
    if (found.empty() && shell_failed)
        std::printf("\nNothing atrium uses is out of step, so this is a bug in atrium: its log is in\n"
                    "journalctl --user -b -t atrium-session. Please report it at https://github.com/mKonic/atrium/issues\n");
    int rc = found.empty() ? 0 : 1;
    if (mode == "--fix")
        rc = fix(found, c, release, newer);
    if (shell_failed) {
        std::printf("\nPress Enter to close.");
        std::fflush(stdout);
        std::string l;
        std::getline(std::cin, l);
    }
    return rc;
}
