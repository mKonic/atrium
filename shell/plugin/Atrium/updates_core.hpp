#pragma once
// Software updates' arithmetic, without Qt: PackageKit's package ids and
// numbers, and what the Updates page says about them.

#include <string>
#include <string_view>
#include <vector>

namespace atrium::updates {

// PackageKit's numbers (pk-enum.h).
namespace pk {
constexpr unsigned kFilterNone = 1u << 1;     // PK_FILTER_ENUM_NONE (1), as a bit
constexpr unsigned kOnlyTrusted = 1u << 1;   // PK_TRANSACTION_FLAG_ENUM_ONLY_TRUSTED
constexpr unsigned kExitSuccess = 1, kExitCancelled = 3;
constexpr unsigned kInfoSecurity = 8, kInfoImportant = 7;
constexpr unsigned kInfoDownloading = 10, kInfoUpdating = 11, kInfoInstalling = 12, kInfoRemoving = 13,
                   kInfoCleanup = 14, kInfoFinished = 18, kInfoPreparing = 21, kInfoDecompressing = 22;
constexpr unsigned kRestartSession = 3, kRestartSystem = 4, kRestartSecuritySession = 5,
                   kRestartSecuritySystem = 6;
constexpr unsigned kRoleInstallFiles = 10, kRoleInstallPackages = 11, kRoleRefreshCache = 13, kRoleRemovePackages = 14,
                   kRoleUpdatePackages = 22, kRoleUpgradeSystem = 33;
} // namespace pk

struct PackageId {
    std::string name, version, arch, repo;
};

// "firefox;131.0-1;x86_64;extra" → its parts; empty name when it isn't one.
PackageId parse_package_id(std::string_view id);

// What's going on with a package while updating ("Downloading", "Installing"),
// "" for the rest.
std::string doing(unsigned info);

// Updating these needs a restart to take effect, whatever the backend says
// (pacman's doesn't say): the kernel, its modules and the system's core.
bool needs_restart(std::string_view name);

// "3 updates, 1 for security"; "No updates" for none.
std::string summary(int count, int security);

// checkupdates, `paru -Qua`, `yay -Qua`: "name 1.0-1 -> 1.1-1" per line
// (anything else is skipped, colour codes too).
struct Upgrade {
    std::string name, from, to;
};
std::vector<Upgrade> parse_upgrades(std::string_view text);

// pacman's "(3/10) upgrading firefox" progress lines: how far, and what.
struct Step {
    int done = 0, total = 0;
    std::string what;  // "Upgrading firefox"
};
bool parse_pacman_step(std::string_view line, Step& step);

// atrium's own releases: a tag "v0.2.0" and the running build's name as
// version.sh gives it ("v0.1.0", "v0.1.0-5-gabc1234", a bare hash before any
// tag). True when the release is newer than the build: a build past a tag is
// newer than that tag, not than the next; one before any tag is older than all.
bool release_newer(std::string_view tag, std::string_view build);
// semver order of two tags ("v1.2.0" < "v1.10.0", "v1.4.0-rc.1" < "v1.4.0"): <0, 0, >0.
int compare_tags(std::string_view a, std::string_view b);
// The package version a build installs as (packaging/arch/PKGBUILD's pkgver):
// "v0.1.0" → "0.1.0", "v0.1.0-5-gabc" → "0.1.0.r5.gabc", "v1.4.0-rc.1" →
// "1.4.0rc.1", a bare hash → "0.rN.hash" (N = build - 10000).
std::string pkgver(std::string_view name, long build);

} // namespace atrium::updates
