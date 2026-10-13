#pragma once
#include "reveal_core.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

namespace atrium {

// Whether a panel that can hide is out (reveal_core.hpp), for the menu bar
// and the Dock alike: each says when it hides and what keeps it, and shows
// itself by `out`.
class PanelReveal : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hides READ hides WRITE setHides NOTIFY inputsChanged)
    Q_PROPERTY(bool edge READ edge WRITE setEdge NOTIFY inputsChanged)
    Q_PROPERTY(bool hovered READ hovered WRITE setHovered NOTIFY inputsChanged)
    Q_PROPERTY(bool held READ held WRITE setHeld NOTIFY inputsChanged)
    Q_PROPERTY(bool out READ out NOTIFY outChanged)

public:
    explicit PanelReveal(QObject* parent = nullptr);

    bool hides() const { return in_.hides; }
    bool edge() const { return in_.edge; }
    bool hovered() const { return in_.hovered; }
    bool held() const { return in_.held; }
    void setHides(bool v) { set(in_.hides, v); }
    void setEdge(bool v) { set(in_.edge, v); }
    void setHovered(bool v) { set(in_.hovered, v); }
    void setHeld(bool v) { set(in_.held, v); }
    bool out() const { return out_; }

    // The key that brings it out for a while, or sends it away.
    Q_INVOKABLE void summon();

signals:
    void inputsChanged();
    void outChanged();

private:
    void set(bool& field, bool v);
    void settle(bool out);

    reveal::Reveal core_;
    reveal::Inputs in_;
    bool out_ = true;
    QElapsedTimer clock_;
    QTimer wake_;
};

} // namespace atrium
