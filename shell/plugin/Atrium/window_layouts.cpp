#include "window_layouts.hpp"

#include "apps.hpp"
#include "compositor.hpp"

#include <QDateTime>

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

constexpr qint64 kLaunchWaitMs = 15000;  // an app that takes longer is left to open where it would

QVariantMap outputNamed(const QString& name) {
    Compositor* c = Compositor::instance();
    for (const QVariant& v : c->outputs())
        if (v.toMap().value("name") == name)
            return v.toMap();
    return c->focusedOutput().toMap();
}

} // namespace

WindowLayouts* WindowLayouts::instance() {
    static auto* self = new WindowLayouts;
    return self;
}

WindowLayouts::WindowLayouts() {
    connect(Compositor::instance(), &Compositor::windowsChanged, this, &WindowLayouts::windowsChanged);
}

QVariantList WindowLayouts::layouts() const {
    QStringList order;
    QHash<QString, int> counts;
    for (const QVariant& v : Compositor::instance()->records("layout_windows")) {
        const QString name = v.toMap().value("layout").toString();
        if (!counts.contains(name))
            order.push_back(name);
        ++counts[name];
    }
    QVariantList out;
    for (const QString& n : order)
        out.push_back(QVariantMap{{"name", n}, {"windows", counts.value(n)}});
    return out;
}

QString WindowLayouts::capture() {
    Compositor* c = Compositor::instance();
    const QVariantMap out = c->focusedOutput().toMap();
    const QString output = out.value("name").toString();
    const QString space = QString::number(c->activeSpace(output));
    const QVariantMap usable = out.value("usable").toMap();
    const double ux = usable.value("x").toDouble(), uy = usable.value("y").toDouble();
    const double uw = std::max(1.0, usable.value("width").toDouble()), uh = std::max(1.0, usable.value("height").toDouble());

    QVariantList rows;
    for (const QVariant& v : c->windows()) {
        const QVariantMap w = v.toMap();
        if (w.value("output") != output || w.value("space").toString() != space || w.value("minimized").toBool() ||
            w.value("skip_taskbar").toBool() || w.value("secret").toBool())
            continue;
        const QVariantMap g = w.value("geometry").toMap();
        rows.push_back(QVariantMap{
            {"app", w.value("app_id")},
            {"output", output},
            {"x", (g.value("x").toDouble() - ux) / uw},
            {"y", (g.value("y").toDouble() - uy) / uh},
            {"width", g.value("width").toDouble() / uw},
            {"height", g.value("height").toDouble() / uh},
            {"maximized", w.value("maximized").toBool() || w.value("fullscreen").toBool()},
        });
    }
    if (rows.isEmpty())
        return {};
    // "Layout", "Layout 2", ...
    QStringList taken;
    for (const QVariant& l : layouts())
        taken.push_back(l.toMap().value("name").toString());
    QString name = "Layout";
    for (int i = 2; taken.contains(name); ++i)
        name = QString("Layout %1").arg(i);
    for (QVariant& r : rows) {
        QVariantMap m = r.toMap();
        m["layout"] = name;
        c->addRecord("layout_windows", m);
    }
    return name;
}

void WindowLayouts::place(int window, const QVariantMap& row, const QVariantMap& info) const {
    Compositor* c = Compositor::instance();
    const QVariantMap out = outputNamed(row.value("output").toString());
    const QString output = out.value("name").toString();
    // Into the space showing on its screen, from wherever it was.
    const int space = c->activeSpace(output);
    if (info.value("output") != output || info.value("space").toString() != QString::number(space))
        if (output == c->focusedOutput().toMap().value("name").toString() && space > 0)
            c->windowRequest(window, "to_space", {{"number", space}});
    if (info.value("minimized").toBool())
        c->windowRequest(window, "minimize", {{"value", false}});
    if (row.value("maximized").toBool()) {
        c->windowRequest(window, "maximize", {{"value", true}});
        return;
    }
    const QVariantMap u = out.value("usable").toMap();
    const double ux = u.value("x").toDouble(), uy = u.value("y").toDouble();
    const double uw = u.value("width").toDouble(), uh = u.value("height").toDouble();
    c->windowRequest(window, "frame",
                     {{"x", int(std::lround(ux + row.value("x").toDouble() * uw))},
                      {"y", int(std::lround(uy + row.value("y").toDouble() * uh))},
                      {"width", int(std::lround(row.value("width").toDouble() * uw))},
                      {"height", int(std::lround(row.value("height").toDouble() * uh))}});
}

void WindowLayouts::run(const QString& name, const EntryIndex& apps) {
    Compositor* c = Compositor::instance();
    const QVariantList windows = c->windows();
    known_.clear();
    for (const QVariant& v : windows)
        known_.insert(v.toMap().value("id").toInt());
    pending_.clear();
    running_ = name;
    QSet<int> used;
    int placedNow = 0;
    for (const QVariant& r : c->records("layout_windows")) {
        const QVariantMap row = r.toMap();
        if (row.value("layout") != name)
            continue;
        const QString app = row.value("app").toString();
        // A window of that app not already given a place, most recent first.
        QVariantMap match;
        for (const QVariant& v : windows) {
            const QVariantMap w = v.toMap();
            if (w.value("app_id").toString().compare(app, Qt::CaseInsensitive) == 0 &&
                !used.contains(w.value("id").toInt()) && !w.value("skip_taskbar").toBool()) {
                match = w;
                break;
            }
        }
        if (!match.isEmpty()) {
            used.insert(match.value("id").toInt());
            place(match.value("id").toInt(), row, match);
            ++placedNow;
            continue;
        }
        // Not running: start it, and place its window when it shows.
        if (QObject* entry = apps.forApp(app)) {
            QMetaObject::invokeMethod(entry, "execute");
            pending_.push_back({row, QDateTime::currentMSecsSinceEpoch() + kLaunchWaitMs});
        }
    }
    emit placed(name, placedNow);
}

void WindowLayouts::windowsChanged() {
    if (pending_.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    pending_.removeIf([&](const Pending& p) { return p.until < now; });
    for (const QVariant& v : Compositor::instance()->windows()) {
        const QVariantMap w = v.toMap();
        const int id = w.value("id").toInt();
        if (known_.contains(id))
            continue;
        for (qsizetype i = 0; i < pending_.size(); ++i)
            if (w.value("app_id").toString().compare(pending_[i].row.value("app").toString(), Qt::CaseInsensitive) == 0) {
                known_.insert(id);
                place(id, pending_[i].row, w);
                pending_.removeAt(i);
                break;
            }
    }
}

void WindowLayouts::rename(const QString& from, const QString& to) {
    const QString name = to.trimmed();
    if (name.isEmpty() || name == from)
        return;
    for (const QVariant& r : Compositor::instance()->records("layout_windows"))
        if (r.toMap().value("layout") == from)
            Compositor::instance()->setRecord("layout_windows", r.toMap().value("id"), {{"layout", name}});
}

void WindowLayouts::remove(const QString& name) {
    for (const QVariant& r : Compositor::instance()->records("layout_windows"))
        if (r.toMap().value("layout") == name)
            Compositor::instance()->removeRecord("layout_windows", r.toMap().value("id"));
}

} // namespace atrium
