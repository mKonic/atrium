#include "views.hpp"

#include "compositor.hpp"

namespace atrium {

OutputState::OutputState(QObject* parent) : QObject(parent) {
    Compositor* c = Compositor::instance();
    connect(c, &Compositor::windowsChanged, this, &OutputState::update);
    connect(c, &Compositor::spacesChanged, this, &OutputState::update);
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
    if (active == active_ && fullscreen == fullscreen_ && spaces == spaces_)
        return;
    active_ = active;
    fullscreen_ = fullscreen;
    spaces_ = spaces;
    emit changed();
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
