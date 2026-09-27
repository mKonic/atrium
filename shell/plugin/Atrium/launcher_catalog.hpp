#pragma once
// What the palette offers besides apps and the user's own records: its
// commands (screens and shortcuts into atrium), system actions and window
// commands, and running a system action. Each item's id is stable: it keys
// learned ranking, favorites, aliases and shortcuts ("system:sleep").

#include <QString>

#include <span>

namespace atrium {

struct CatalogItem {
    const char* id;
    const char* title;
    const char* glyph;     // Material Symbols
    const char* keywords;  // space-separated, only make it appear
    bool confirm = false;  // asks first (Restart, Empty Trash)
};

// Palette commands: "screen:<name>" opens one of the palette's screens,
// the rest run (see LauncherModel::runCommand).
std::span<const CatalogItem> paletteCommands();
std::span<const CatalogItem> systemActions();
// Window commands: the place action's names (geometry::named_place) plus the
// window manager's own (maximize, restore, next-display, prev-display).
std::span<const CatalogItem> windowCommands();

// Runs a system action; what it did, as the pill says it ("Dark Appearance",
// "Trash Is Already Empty"), or "" when the action shows for itself.
// `noop` is set when there was nothing to do.
QString runSystemAction(const QString& id, bool* noop);

} // namespace atrium
