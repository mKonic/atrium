#include "reveal.hpp"

namespace atrium {

PanelReveal::PanelReveal(QObject* parent) : QObject(parent) {
    clock_.start();
    wake_.setSingleShot(true);
    wake_.setTimerType(Qt::PreciseTimer);
    connect(&wake_, &QTimer::timeout, this, [this] { settle(core_.update(in_, double(clock_.elapsed()))); });
}

void PanelReveal::set(bool& field, bool v) {
    if (field == v)
        return;
    field = v;
    settle(core_.update(in_, double(clock_.elapsed())));
    emit inputsChanged();
}

void PanelReveal::summon() {
    settle(core_.summon(double(clock_.elapsed())));
}

void PanelReveal::peek() {
    settle(core_.peek(double(clock_.elapsed())));
}

void PanelReveal::settle(bool out) {
    if (const auto in = core_.wake_in(double(clock_.elapsed())))
        wake_.start(int(*in));
    else
        wake_.stop();
    if (out == out_)
        return;
    out_ = out;
    emit outChanged();
}

} // namespace atrium
