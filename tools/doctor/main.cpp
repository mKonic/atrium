// atrium-doctor: whether what atrium (and the Qt plugins around it) was built
// against is still what's installed, and fixing it when it isn't: a newer
// release, else the latest atrium built from source; AUR packages rebuilt
// from the AUR. Links nothing but the C++ runtime, so it still runs when
// what atrium uses has moved under it.
//
//   atrium-doctor          what's wrong, and how --fix would fix it
//   atrium-doctor --fix    fix it (asks first)
//   atrium-doctor --brief  one line when something's wrong (pacman's hook)
//   atrium-doctor --notify a notification when something's wrong (login)

#include "doctor_core.hpp"
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

extern char** environ;

namespace fs = std::filesystem;
using namespace atrium::doctor;

namespace {

constexpr const char* kRepo = "https://github.com/mKonic/atrium.git";
constexpr const char* kLatest = "https://api.github.com/repos/mKonic/atrium/releases/latest";

// Runs `argv`, its output captured (stderr too when `errors`); "" when it
// can't start. `status` gets its exit status.
std::string capture(const std::vector<std::string>& argv, int* status = nullptr, bool errors = false) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0)
        return {};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    if (errors)
        posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    else
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    std::vector<char*> args;
    for (const std::string& a : argv)
        args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid;
    const int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    std::string out;
    if (rc == 0) {
        char buf[4096];
        for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;)
            out.append(buf, size_t(n));
        int st = 0;
        waitpid(pid, &st, 0);
        if (status)
            *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    } else if (status) {
        *status = -1;
    }
    close(fds[0]);
    return out;
}

// Runs `argv` on this terminal, in `dir`; true when it succeeds.
bool run(const std::vector<std::string>& argv, const std::string& dir = {}, const std::vector<std::string>& env = {}) {
    std::string line;
    for (const std::string& a : argv)
        line += (line.empty() ? "" : " ") + a;
    std::printf("\n$ %s\n", line.c_str());
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        if (!dir.empty() && chdir(dir.c_str()) != 0)
            _exit(127);
        for (const std::string& e : env)
            putenv(const_cast<char*>(e.c_str()));
        std::vector<char*> args;
        for (const std::string& a : argv)
            args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at);
        std::string l = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        if (!l.empty())
            out.push_back(std::move(l));
        if (nl == std::string::npos)
            break;
        at = nl + 1;
    }
    return out;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    return s;
}

std::string owner(const std::string& file) {
    return trim(capture({"pacman", "-Qqo", file}));
}

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

struct Context {
    std::string atrium;  // the package /usr/bin/atrium belongs to
    std::set<std::string> foreign;  // packages from no repository (pacman -Qm)
};

Source source_of(const Context& c, const std::string& package) {
    if (package == c.atrium)
        return Source::Atrium;
    return c.foreign.contains(package) ? Source::Aur : Source::Repo;
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

} // namespace

int main(int argc, char** argv) {
    std::string mode;
    bool shell_failed = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--fix" || a == "--brief" || a == "--notify") {
            mode = a;
        } else if (a == "--shell-failed") {
            // From atrium, in a terminal, when the shell keeps failing.
            mode = "--fix";
            shell_failed = true;
        } else {
            std::fprintf(stderr,
                         "usage: atrium-doctor [--fix | --brief | --notify]\n"
                         "  (none)    what doesn't work with what's installed, and how --fix fixes it\n"
                         "  --fix     fix it: a newer atrium release, else atrium built from source;\n"
                         "            AUR packages rebuilt; a system update for the rest\n"
                         "  --brief   one line when something's wrong, nothing otherwise\n"
                         "  --notify  a desktop notification when something's wrong\n");
            return a == "-h" || a == "--help" ? 0 : 2;
        }
    }

    Context c;
    if (fs::exists("/usr/bin/atrium"))
        c.atrium = owner("/usr/bin/atrium");
    for (const std::string& p : lines(capture({"pacman", "-Qqm"})))
        c.foreign.insert(p);
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
