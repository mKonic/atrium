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
        for (const auto& l : list.layouts) {
            const QString code = QString::fromStdString(l.code), name = QString::fromStdString(l.description);
            layouts_.append(QVariantMap{{"value", code}, {"label", name}});
            names_.insert(code, name);
            all_.append(QVariantMap{{"layout", code}, {"variant", QString()}, {"label", name}});
        }
        for (const auto& v : list.variants) {
            const QString layout = QString::fromStdString(v.layout), variant = QString::fromStdString(v.code);
            const QString name = QString::fromStdString(v.description);
            names_.insert(layout + "(" + variant + ")", name);
            all_.append(QVariantMap{{"layout", layout}, {"variant", variant}, {"label", name}});
        }
        std::sort(all_.begin(), all_.end(), [&](const QVariant& a, const QVariant& b) {
            return order.compare(a.toMap().value("label").toString(), b.toMap().value("label").toString()) < 0;
        });
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

QList<KeyboardLayouts::Source> KeyboardLayouts::read() const {
    Compositor* c = Compositor::instance();
    QString layouts = c->setting("keyboard.layout", QString()).toString();
    QString variants = c->setting("keyboard.variant", QString()).toString();
    if (layouts.isEmpty()) {
        layouts = QString::fromStdString(system_keyboard().layout);
        variants = QString::fromStdString(system_keyboard().variant);
    }
    if (layouts.isEmpty())
        layouts = "us";
    const QStringList l = layouts.split(','), v = variants.split(',');
    QList<Source> out;
    for (qsizetype i = 0; i < l.size(); ++i)
        if (!l[i].trimmed().isEmpty())
            out.append({l[i].trimmed(), i < v.size() ? v[i].trimmed() : QString()});
    return out;
}

void KeyboardLayouts::write(const QList<Source>& sources) {
    QStringList l, v;
    for (const Source& s : sources) {
        l.append(s.layout);
        v.append(s.variant);
    }
    Compositor::instance()->setSetting("keyboard.layout", l.join(','));
    // No variants at all: an empty setting rather than ",,".
    Compositor::instance()->setSetting("keyboard.variant",
                                       std::ranges::all_of(v, &QString::isEmpty) ? QString() : v.join(','));
}

QString KeyboardLayouts::label(const Source& s) const {
    const QString key = s.variant.isEmpty() ? s.layout : s.layout + "(" + s.variant + ")";
    return names_.value(key, key);
}

QVariantList KeyboardLayouts::sources() const {
    QVariantList out;
    for (const Source& s : read())
        out.append(QVariantMap{{"layout", s.layout}, {"variant", s.variant}, {"label", label(s)}});
    return out;
}

QVariantList KeyboardLayouts::find(const QString& query) const {
    const QString q = query.trimmed();
    QVariantList out;
    for (const QVariant& v : all_)
        if (q.isEmpty() || v.toMap().value("label").toString().contains(q, Qt::CaseInsensitive))
            out.append(v);
    return out;
}

void KeyboardLayouts::add(const QString& layout, const QString& variant) {
    QList<Source> s = read();
    for (const Source& e : s)
        if (e.layout == layout && e.variant == variant)
            return;
    s.append({layout, variant});
    write(s);
}

void KeyboardLayouts::remove(int index) {
    QList<Source> s = read();
    if (index < 0 || index >= s.size() || s.size() == 1)
        return;
    s.removeAt(index);
    write(s);
}

void KeyboardLayouts::moveUp(int index) {
    QList<Source> s = read();
    if (index <= 0 || index >= s.size())
        return;
    s.swapItemsAt(index, index - 1);
    write(s);
}

void KeyboardLayouts::set(const QString& code) {
    Compositor::instance()->setSetting("keyboard.layout", code);
    Compositor::instance()->setSetting("keyboard.variant", QString());
}

} // namespace atrium
