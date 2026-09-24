#pragma once
// Where a panel's Liquid Glass is, for atrium to draw it (atrium-glass-v1):
// a GlassShape filling a Rectangle is that Rectangle's glass, with its
// radius. Every frame, the shapes of a window go to atrium with the buffer
// they belong to, so the glass moves, grows and fades with its panel.

#include <QPointer>
#include <QQuickItem>
#include <QVector>

struct atrium_glass_v1;

namespace atrium::shell {

class GlassShape : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(qreal radius READ radius WRITE setRadius NOTIFY radiusChanged)

public:
    explicit GlassShape(QQuickItem* parent = nullptr);
    ~GlassShape() override;

    qreal radius() const { return radius_; }
    void setRadius(qreal r);

signals:
    void radiusChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData& data) override;
    void geometryChange(const QRectF& now, const QRectF& before) override;

private:
    void attach(QQuickWindow* window);
    void changed();

    qreal radius_ = 0;
    QPointer<QQuickWindow> window_;
};

} // namespace atrium::shell
