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

} // namespace atrium::updates
