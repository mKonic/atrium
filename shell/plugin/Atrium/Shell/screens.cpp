#include "screens.hpp"

#include <QGuiApplication>

namespace atrium::shell {

ShellScreen::ShellScreen(QScreen* screen, QObject* parent) : QObject(parent), screen_(screen) {
    connect(screen, &QScreen::geometryChanged, this, &ShellScreen::geometryChanged);
    connect(screen, &QScreen::physicalDotsPerInchChanged, this, &ShellScreen::geometryChanged);
}

Screens* Screens::instance() {
    static auto* self = new Screens;
    return self;
}

Screens::Screens() {
    for (QScreen* s : QGuiApplication::screens())
        add(s);
    connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen* s) {
        add(s);
        emit changed();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen* s) {
        remove(s);
        emit changed();
    });
}

void Screens::add(QScreen* screen) {
    list_.append(new ShellScreen(screen, this));
}

void Screens::remove(QScreen* screen) {
    for (qsizetype i = 0; i < list_.size(); ++i) {
        if (list_[i]->screen() == screen) {
            // Deleted later: windows on it hear screensChanged first.
            list_.takeAt(i)->deleteLater();
            return;
        }
    }
}

ShellScreen* Screens::forScreen(QScreen* screen) const {
    for (ShellScreen* s : list_)
        if (s->screen() == screen)
            return s;
    return nullptr;
}

} // namespace atrium::shell
