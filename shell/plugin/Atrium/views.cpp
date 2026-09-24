#include "views.hpp"

#include "compositor.hpp"

namespace atrium {

OutputState::OutputState(QObject* parent) : QObject(parent) {
    Compositor* c = Compositor::instance();
    connect(c, &Compositor::windowsChanged, this, &OutputState::update);
    connect(c, &Compositor::spacesChanged, this, &OutputState::update);
    connect(c, &Compositor::edgeReached, this, [this](const QString& output, const QString& edge) {
        if (output == name_ && edge != edge_) {
            edge_ = edge;
            emit edgeChanged();
        }
    });
}

void OutputState::setName(const QString& name) {
    if (name == name_)
        return;
    name_ = name;
    emit nameChanged();
    update();
}

void OutputState::update() {
    Compositor* c = Compositor::instance();
    const int active = c->activeSpace(name_);
    const bool fullscreen = c->fullscreenOn(name_);
    const QVariantList spaces = c->spacesOn(name_);
    const QVariantList secrets = collectSecrets();
    bool tiled = false;
    for (const QVariant& v : spaces)
        if (v.toMap().value("number").toInt() == active)
            tiled = v.toMap().value("tiled").toBool();
    if (active == active_ && fullscreen == fullscreen_ && tiled == tiled_ && spaces == spaces_ && secrets == secrets_)
        return;
    active_ = active;
    fullscreen_ = fullscreen;
    tiled_ = tiled;
    spaces_ = spaces;
    secrets_ = secrets;
    emit changed();
}

QVariantList OutputState::collectSecrets() const {
    Compositor* c = Compositor::instance();
    QVariantList out;
    for (const QVariant& v : c->spaces()) {
        const QVariantMap s = v.toMap();
        if (!s.value("secret").toBool())
            continue;
        const QString name = s.value("label").toString();
        const bool shown = s.value("shown").toBool() && s.value("output").toString() == name_;
        QStringList apps;
        for (const QVariant& w : c->windows()) {  // most recently used first
            const QVariantMap m = w.toMap();
            const QString app = m.value("app_id").toString();
            if (m.value("secret").toBool() && m.value("space").toString() == name && !apps.contains(app))
                apps.push_back(app);
        }
        if (apps.isEmpty() && !shown)
            continue;
        out.push_back(QVariantMap{{"name", name}, {"shown", shown}, {"apps", apps}});
    }
    return out;
}

SpaceWindows::SpaceWindows(QObject* parent) : QObject(parent) {
    connect(Compositor::instance(), &Compositor::windowsChanged, this, &SpaceWindows::update);
}

void SpaceWindows::setOutput(const QString& output) {
    if (output == output_)
        return;
    output_ = output;
    emit outputChanged();
    update();
}

void SpaceWindows::setNumber(int number) {
    if (number == number_)
        return;
    number_ = number;
    emit numberChanged();
    update();
}

void SpaceWindows::update() {
    const QVariantList windows = Compositor::instance()->windowsOn(output_, number_);
    if (windows == windows_)
        return;
    windows_ = windows;
    emit windowsChanged();
}

} // namespace atrium
