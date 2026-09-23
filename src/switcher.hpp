#pragma once
#include "wlr.hpp"

#include <memory>
#include <vector>

namespace atrium {

class Server;
class View;
class WindowCopy;

// Alt+Tab: hold the modifier and tap Tab to walk the windows of the current
// space, most recently used first; let go to switch to the one picked. A row
// of live previews shows up if the modifier is held for a moment, so a quick
// tap just flips to the previous window without anything flashing.
class Switcher {
public:
    explicit Switcher(Server& server);
    ~Switcher();
    Switcher(const Switcher&) = delete;
    Switcher& operator=(const Switcher&) = delete;

    bool active() const { return active_; }

    // A switcher binding fired: start, or step `direction` further. `hold` is
    // the binding's modifiers; releasing them all picks.
    void step(int direction, uint32_t hold);
    void modifiers(uint32_t mods);
    void key(xkb_keysym_t sym);  // presses while active
    void button(double lx, double ly, bool pressed);
    void motion(double lx, double ly);
    void cancel();

    void view_changed(View* view);
    void view_unmapped(View* view);

private:
    struct Item {
        View* view;
        wlr_scene_tree* tree;
        std::unique_ptr<WindowCopy> copy;
        wlr_box box;  // layout coordinates
    };

    void commit();
    void show();
    void hide();
    void layout();
    void select(int index);
    void render_title();
    int item_at(double lx, double ly) const;

    Server& server_;
    bool active_ = false;
    bool shown_ = false;
    uint32_t hold_ = 0;
    std::vector<View*> views_;  // candidates, most recently used first
    int index_ = 0;

    wl_event_source* delay_ = nullptr;
    wlr_scene_tree* root_ = nullptr;
    wlr_scene_blur* blur_ = nullptr;
    wlr_scene_rect* panel_ = nullptr;
    wlr_scene_rect* selection_ = nullptr;
    wlr_scene_buffer* title_ = nullptr;
    std::vector<std::unique_ptr<Item>> items_;
    wlr_box panel_box_{};
};

} // namespace atrium
