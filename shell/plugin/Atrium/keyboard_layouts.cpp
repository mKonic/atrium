#include "keyboard_layouts.hpp"

#include "compositor.hpp"
#include "keyboard_conf.hpp"
#include "keyboard_core.hpp"

#include <QCollator>
#include <QFile>

#include <algorithm>

namespace atrium {

KeyboardLayouts::KeyboardLayouts(QObject* parent) : QObject(parent) {
    QFile f("/usr/share/X11/xkb/rules/evdev.lst");
    if (f.open(QIODevice::ReadOnly)) {
        keyboard::XkbList list = keyboard::parse_xkb_list(f.readAll().toStdString());
        QCollator order;
        std::sort(list.layouts.begin(), list.layouts.end(), [&](const auto& a, const auto& b) {
            return order.compare(QString::fromStdString(a.description), QString::fromStdString(b.description)) < 0;
        });
        for (const auto& l : list.layouts)
            layouts_.append(QVariantMap{{"value", QString::fromStdString(l.code)},
                                        {"label", QString::fromStdString(l.description)}});
    }
    connect(Compositor::instance(), &Compositor::settingsChanged, this, &KeyboardLayouts::currentChanged);
}

QString KeyboardLayouts::current() const {
    QString layouts = Compositor::instance()->setting("keyboard.layout", QString()).toString();
    if (layouts.isEmpty())
        layouts = QString::fromStdString(system_keyboard().layout);
    const QString first = layouts.section(',', 0, 0).trimmed();
    return first.isEmpty() ? QStringLiteral("us") : first;
}

void KeyboardLayouts::set(const QString& code) {
    Compositor::instance()->setSetting("keyboard.layout", code);
    Compositor::instance()->setSetting("keyboard.variant", QString());
}

} // namespace atrium
