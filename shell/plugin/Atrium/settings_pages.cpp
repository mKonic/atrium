#include "settings_pages.hpp"

#include "compositor.hpp"
#include "search.hpp"

#include <algorithm>
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
        if (byPage.contains(p.name))
            pages.push_back(QVariantMap{{"name", p.name}, {"icon", p.icon}, {"color", p.color}, {"special", false}});
    for (const QString& name : order)
        if (std::ranges::none_of(kPages, [&](const PageInfo& p) { return name == p.name; }))
            pages.push_back(QVariantMap{{"name", name}, {"icon", "settings"}, {"color", "#8e8e93"}, {"special", false}});
    pages_ = pages;
    byPage_ = byPage;
    emit changed();
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
