#pragma once
// The screenshot's editor canvas: the picture fitted into the item, and
// marks drawn on it with the pointer: arrows, boxes, blurred (pixelated)
// patches, and a crop. Saving writes them into the file.
//
//   MarkupCanvas { source: "/path.png"; tool: "arrow"; color: "#ff3b30" }

#include <QColor>
#include <QImage>
#include <QList>
#include <QQuickPaintedItem>
#include <QRect>

namespace atrium {

class MarkupCanvas : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged)  // arrow, box, blur, crop
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(bool edited READ edited NOTIFY editedChanged)
    Q_PROPERTY(QSize imageSize READ imageSize NOTIFY sourceChanged)

public:
    explicit MarkupCanvas(QQuickItem* parent = nullptr);

    QString source() const { return source_; }
    void setSource(const QString& file);
    QString tool() const { return tool_; }
    void setTool(const QString& tool);
    QColor color() const { return color_; }
    void setColor(const QColor& c);
    bool edited() const { return !marks_.isEmpty() || !crop_.isNull(); }
    QSize imageSize() const { return image_.size(); }

    Q_INVOKABLE void undo();
    // The marks and crop into the file (and the clipboard); true when saved.
    Q_INVOKABLE bool save();

    void paint(QPainter* painter) override;

signals:
    void sourceChanged();
    void toolChanged();
    void colorChanged();
    void editedChanged();

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    struct Mark {
        QString tool;
        QPointF from, to;  // picture pixels
        QColor color;
    };
    QRectF fitted() const;               // where the picture is drawn in the item
    QPointF toImage(QPointF item) const;
    void draw(QPainter& p, const Mark& m, const QImage& under) const;
    QImage rendered() const;             // picture and marks, cropped

    QString source_, tool_ = "arrow";
    QColor color_ = QColor("#ff3b30");
    QImage image_;
    QList<Mark> marks_;
    QRect crop_;          // picture pixels; null: none
    QList<QRect> crops_;  // earlier crops, for undo
    bool dragging_ = false;
    Mark current_;
};

} // namespace atrium
