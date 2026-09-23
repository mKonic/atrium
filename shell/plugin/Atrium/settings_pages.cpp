#include "settings_pages.hpp"

#include "compositor.hpp"
#include "search.hpp"

#include <algorithm>
#include <map>
#include <vector>

namespace atrium {

namespace {

struct PageInfo {
    const char* name;
    const char* icon;   // Material Symbols
    const char* color;  // the square behind the icon
};

// System Settings' order: how things look, then what you touch, then the
// machine. Pages the schema has that aren't listed go after these.
constexpr PageInfo kPages[] = {
    {"Appearance", "palette", "#5e5ce6"},
    {"Menu Bar", "toolbar", "#8e8e93"},
    {"Dock", "dock_to_bottom", "#8e8e93"},
    {"Desktop", "desktop_windows", "#0a84ff"},
    {"Apps", "apps", "#0a84ff"},
    {"Windows", "select_window", "#0a84ff"},
    {"Displays", "brightness_high", "#0a84ff"},
    {"Notifications", "notifications", "#ff453a"},
    {"Keyboard", "keyboard", "#8e8e93"},
    {"Keyboard Shortcuts", "keyboard_command_key", "#8e8e93"},
    {"Mouse & Touchpad", "mouse", "#8e8e93"},
    {"Power", "bolt", "#30d158"},
    {"Screen Recording", "screen_record", "#ff453a"},
    {"Privacy & Security", "back_hand", "#0a84ff"},
    {"Session", "power_settings_new", "#636366"},
};

} // namespace

SettingsPages::SettingsPages(QObject* parent) : QObject(parent) {
    connect(Compositor::instance(), &Compositor::schemaChanged, this, &SettingsPages::rebuild);
    rebuild();
}

void SettingsPages::rebuild() {
    QVariantMap byPage;
    QStringList order;
    for (const QVariant& v : Compositor::instance()->schema()) {
        const QVariantMap s = v.toMap();
        const QString page = s.value("page").toString();
        QVariantList list = byPage.value(page).toList();
        list.push_back(s);
        byPage[page] = list;
        if (!order.contains(page))
            order.push_back(page);
    }
    QVariantList pages;
    pages.push_back(QVariantMap{{"name", "About"}, {"icon", "info"}, {"color", "#8e8e93"}, {"special", true}});
    for (const PageInfo& p : kPages)
        if (byPage.contains(p.name) || QString(p.name) == "Apps")
            pages.push_back(QVariantMap{{"name", p.name}, {"icon", p.icon}, {"color", p.color},
                                        {"special", !byPage.contains(p.name)}});
    for (const QString& name : order)
        if (std::ranges::none_of(kPages, [&](const PageInfo& p) { return name == p.name; }))
            pages.push_back(QVariantMap{{"name", name}, {"icon", "settings"}, {"color", "#8e8e93"}, {"special", false}});
    pages_ = pages;
    byPage_ = byPage;
    emit changed();
}

QString SettingsPages::chord(int key, int modifiers, const QString& modifier) const {
    static const std::map<int, const char*> names = {
        {Qt::Key_Return, "Return"}, {Qt::Key_Enter, "KP_Enter"}, {Qt::Key_Tab, "Tab"}, {Qt::Key_Backtab, "Tab"},
        {Qt::Key_Space, "space"}, {Qt::Key_Backspace, "BackSpace"}, {Qt::Key_Delete, "Delete"},
        {Qt::Key_Insert, "Insert"}, {Qt::Key_Home, "Home"}, {Qt::Key_End, "End"}, {Qt::Key_PageUp, "Page_Up"},
        {Qt::Key_PageDown, "Page_Down"}, {Qt::Key_Left, "Left"}, {Qt::Key_Right, "Right"}, {Qt::Key_Up, "Up"},
        {Qt::Key_Down, "Down"}, {Qt::Key_Print, "Print"}, {Qt::Key_Escape, "Escape"},
        {Qt::Key_Comma, "comma"}, {Qt::Key_Period, "period"}, {Qt::Key_Slash, "slash"},
        {Qt::Key_Backslash, "backslash"}, {Qt::Key_Semicolon, "semicolon"}, {Qt::Key_Apostrophe, "apostrophe"},
        {Qt::Key_BracketLeft, "bracketleft"}, {Qt::Key_BracketRight, "bracketright"}, {Qt::Key_Minus, "minus"},
        {Qt::Key_Equal, "equal"}, {Qt::Key_QuoteLeft, "grave"}, {Qt::Key_Plus, "plus"},
        {Qt::Key_VolumeUp, "XF86AudioRaiseVolume"}, {Qt::Key_VolumeDown, "XF86AudioLowerVolume"},
        {Qt::Key_VolumeMute, "XF86AudioMute"}, {Qt::Key_MicMute, "XF86AudioMicMute"},
        {Qt::Key_MediaPlay, "XF86AudioPlay"}, {Qt::Key_MediaTogglePlayPause, "XF86AudioPlay"},
        {Qt::Key_MediaPause, "XF86AudioPause"}, {Qt::Key_MediaNext, "XF86AudioNext"},
        {Qt::Key_MediaPrevious, "XF86AudioPrev"}, {Qt::Key_MonBrightnessUp, "XF86MonBrightnessUp"},
        {Qt::Key_MonBrightnessDown, "XF86MonBrightnessDown"}, {Qt::Key_Calculator, "XF86Calculator"},
    };
    QString name;
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        name = QChar(char('A' + (key - Qt::Key_A)));
    else if (key >= Qt::Key_0 && key <= Qt::Key_9)
        name = QChar(char('0' + (key - Qt::Key_0)));
    else if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        name = QString("F%1").arg(key - Qt::Key_F1 + 1);
    else if (auto it = names.find(key); it != names.end())
        name = it->second;
    else
        return {};  // a modifier alone, or a key atrium can't name

    struct Mod {
        Qt::KeyboardModifier qt;
        const char* id;     // as the modifier setting names it
        const char* label;  // as a shortcut is written
    };
    const Mod mods[] = {{Qt::MetaModifier, "super", "Super"}, {Qt::ControlModifier, "ctrl", "Ctrl"},
                        {Qt::AltModifier, "alt", "Alt"}, {Qt::ShiftModifier, "shift", "Shift"}};
    QStringList parts;
    // The shortcut modifier first, written as Mod so it follows the setting.
    for (const Mod& m : mods)
        if ((modifiers & m.qt) && modifier == m.id)
            parts.push_back("Mod");
    for (const Mod& m : mods)
        if ((modifiers & m.qt) && modifier != m.id)
            parts.push_back(m.label);
    parts.push_back(name);
    return parts.join('+');
}

QVariantList SettingsPages::search(const QString& query) const {
    const std::string q = query.trimmed().toLower().toStdString();
    if (q.empty())
        return {};
    struct Hit {
        int score;
        QVariant setting;
    };
    std::vector<Hit> hits;
    for (const QVariant& v : Compositor::instance()->schema()) {
        const QVariantMap s = v.toMap();
        const auto text = [&](const char* key) { return s.value(key).toString().toLower().toStdString(); };
        const int score = std::max({search::score(q, text("title")), search::score(q, text("page")) * 8 / 10,
                                    search::score(q, text("description")) * 6 / 10});
        if (score >= 150)
            hits.push_back({score, v});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    QVariantList out;
    for (const Hit& h : hits)
        out.push_back(h.setting);
    return out;
}

} // namespace atrium
