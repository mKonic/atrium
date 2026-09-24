#include "markup.hpp"

#include <QFile>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>

#include <algorithm>
#include <cmath>

namespace atrium {

MarkupCanvas::MarkupCanvas(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton);
    setAntialiasing(true);
}

void MarkupCanvas::setSource(const QString& file) {
    if (file == source_)
        return;
    source_ = file;
    image_ = QImage(file).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    marks_.clear();
    crop_ = {};
    crops_.clear();
    emit sourceChanged();
    emit editedChanged();
    update();
}

void MarkupCanvas::setTool(const QString& tool) {
    if (tool == tool_)
        return;
    tool_ = tool;
    emit toolChanged();
}

void MarkupCanvas::setColor(const QColor& c) {
    if (c == color_)
        return;
    color_ = c;
    emit colorChanged();
}

QRectF MarkupCanvas::fitted() const {
    if (image_.isNull() || width() <= 0 || height() <= 0)
        return {};
    // Shown as cropped: the crop fills the space.
    const QRectF shown = crop_.isNull() ? QRectF(image_.rect()) : QRectF(crop_);
    const qreal s = std::min({width() / shown.width(), height() / shown.height(), 1.0});
    const QSizeF size = shown.size() * s;
    return {(width() - size.width()) / 2, (height() - size.height()) / 2, size.width(), size.height()};
}

QPointF MarkupCanvas::toImage(QPointF item) const {
    const QRectF f = fitted();
    const QRectF shown = crop_.isNull() ? QRectF(image_.rect()) : QRectF(crop_);
    if (f.isEmpty())
        return {};
    // Kept on the picture, so a drag past its edge ends at the edge.
    const qreal x = shown.x() + (item.x() - f.x()) * shown.width() / f.width();
    const qreal y = shown.y() + (item.y() - f.y()) * shown.height() / f.height();
    return {std::clamp(x, shown.left(), shown.right()), std::clamp(y, shown.top(), shown.bottom())};
}

void MarkupCanvas::draw(QPainter& p, const Mark& m, const QImage& under) const {
    // Line widths in picture pixels, scaled with the picture's size.
    const qreal w = std::max(3.0, std::min(image_.width(), image_.height()) / 200.0);
    if (m.tool == "arrow") {
        const QLineF line(m.from, m.to);
        if (line.length() < 1)
            return;
        p.setPen(QPen(m.color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const qreal head = std::min(line.length() * 0.5, w * 5);
        const qreal a = std::atan2(line.dy(), line.dx());
        const QPointF tip = m.to;
        const QPointF l = tip - QPointF(std::cos(a - 0.45) * head, std::sin(a - 0.45) * head);
        const QPointF r = tip - QPointF(std::cos(a + 0.45) * head, std::sin(a + 0.45) * head);
        p.drawLine(QLineF(m.from, (l + r) / 2));
        QPainterPath arrow;
        arrow.moveTo(tip);
        arrow.lineTo(l);
        arrow.lineTo(r);
        arrow.closeSubpath();
        p.setBrush(m.color);
        p.drawPath(arrow);
        p.setBrush(Qt::NoBrush);
    } else if (m.tool == "box") {
        p.setPen(QPen(m.color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(m.from, m.to).normalized(), w * 1.5, w * 1.5);
    } else if (m.tool == "blur") {
        // Pixelated: down to blocks and back up, unreadable.
        const QRect r = QRectF(m.from, m.to).normalized().toAlignedRect().intersected(under.rect());
        if (r.width() < 2 || r.height() < 2)
            return;
        const int block = std::max(8, int(w * 3));
        const QImage small = under.copy(r).scaled(std::max(1, r.width() / block), std::max(1, r.height() / block),
                                                  Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(r, small);
        p.restore();
    }
}

void MarkupCanvas::paint(QPainter* painter) {
    if (image_.isNull())
        return;
    const QRectF f = fitted();
    const QRectF shown = crop_.isNull() ? QRectF(image_.rect()) : QRectF(crop_);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->setRenderHint(QPainter::Antialiasing);
    painter->save();
    painter->translate(f.topLeft());
    painter->scale(f.width() / shown.width(), f.height() / shown.height());
    painter->translate(-shown.topLeft());
    painter->setClipRect(shown);
    // Marks draw over the picture as it is at that point, so a blur over
    // an arrow hides the arrow too.
    QImage layer = image_;
    {
        QPainter lp(&layer);
        lp.setRenderHint(QPainter::Antialiasing);
        for (const Mark& m : marks_)
            draw(lp, m, layer);
        if (dragging_ && current_.tool != "crop")
            draw(lp, current_, layer);
    }
    painter->drawImage(QPointF(0, 0), layer);
    // The crop being dragged: the rest dimmed.
    if (dragging_ && current_.tool == "crop") {
        const QRectF c = QRectF(current_.from, current_.to).normalized();
        QPainterPath outside;
        outside.addRect(shown);
        outside.addRect(c);
        painter->fillPath(outside, QColor(0, 0, 0, 140));
        painter->setPen(QPen(Qt::white, 1.5 * shown.width() / f.width(), Qt::DashLine));
        painter->drawRect(c);
    }
    painter->restore();
}

void MarkupCanvas::mousePressEvent(QMouseEvent* e) {
    if (image_.isNull() || !fitted().contains(e->position()))
        return;
    dragging_ = true;
    current_ = {tool_, toImage(e->position()), toImage(e->position()), color_};
    e->accept();
}

void MarkupCanvas::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_)
        return;
    current_.to = toImage(e->position());
    update();
}

void MarkupCanvas::mouseReleaseEvent(QMouseEvent* e) {
    if (!dragging_)
        return;
    dragging_ = false;
    current_.to = toImage(e->position());
    const QRectF r = QRectF(current_.from, current_.to).normalized();
    if (current_.tool == "crop") {
        const QRect c = r.toAlignedRect().intersected(crop_.isNull() ? image_.rect() : crop_);
        if (c.width() >= 8 && c.height() >= 8) {
            crops_.append(crop_);
            crop_ = c;
            marks_.append({"crop", {}, {}, {}});  // so undo takes it back in order
        }
    } else if (QLineF(current_.from, current_.to).length() >= 4) {
        marks_.append(current_);
    }
    emit editedChanged();
    update();
}

void MarkupCanvas::undo() {
    if (marks_.isEmpty())
        return;
    if (marks_.takeLast().tool == "crop")
        crop_ = crops_.isEmpty() ? QRect() : crops_.takeLast();
    emit editedChanged();
    update();
}

QImage MarkupCanvas::rendered() const {
    QImage layer = image_;
    QPainter lp(&layer);
    lp.setRenderHint(QPainter::Antialiasing);
    for (const Mark& m : marks_)
        draw(lp, m, layer);
    lp.end();
    return crop_.isNull() ? layer : layer.copy(crop_);
}

bool MarkupCanvas::save() {
    if (image_.isNull() || source_.isEmpty())
        return false;
    if (!edited())
        return true;
    if (!rendered().save(source_, "PNG"))
        return false;
    auto* copy = new QProcess;
    QObject::connect(copy, &QProcess::finished, copy, &QObject::deleteLater);
    copy->setStandardInputFile(source_);
    copy->start("wl-copy", {"--type", "image/png"});
    return true;
}

} // namespace atrium
