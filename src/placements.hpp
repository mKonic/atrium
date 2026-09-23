#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace atrium {

// Where an app's window was when it last closed: the floating box (relative
// to its output's top-left, title bar included) and whether it was maximized
// or snapped. Its first window comes back there on the next launch.
struct Placement {
    std::string output;
    int x = 0, y = 0, width = 0, height = 0;
    bool maximized = false;
    uint32_t snapped = 0;
    bool operator==(const Placement&) const = default;
};

// Remembered placements by app id, kept in $XDG_STATE_HOME/atrium. This is
// state, not a setting: it is written as windows close.
class Placements {
public:
    explicit Placements(std::filesystem::path file);

    const Placement* find(const std::string& app_id) const;
    void remember(const std::string& app_id, const Placement& placement);

    static std::filesystem::path default_file();
    const std::filesystem::path& file() const { return file_; }

private:
    void load();
    bool save() const;

    std::filesystem::path file_;
    std::map<std::string, Placement> by_app_;
};

} // namespace atrium
