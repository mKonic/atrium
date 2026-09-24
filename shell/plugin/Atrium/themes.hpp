#pragma once
// What's installed to pick from: icon themes, cursor themes, fonts and
// monospace fonts, each [{value, label}] by name, "Default" (keep what
// apps already use) first.

#include <QObject>
#include <QVariantList>

namespace atrium {

class Themes : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList iconThemes READ iconThemes CONSTANT)
    Q_PROPERTY(QVariantList cursorThemes READ cursorThemes CONSTANT)
    Q_PROPERTY(QVariantList fonts READ fonts CONSTANT)
    Q_PROPERTY(QVariantList monospaceFonts READ monospaceFonts CONSTANT)

public:
    explicit Themes(QObject* parent = nullptr);

    QVariantList iconThemes() const { return icons_; }
    QVariantList cursorThemes() const { return cursors_; }
    QVariantList fonts() const;
    QVariantList monospaceFonts() const;

    // The options a setting picks from ([] for other settings).
    Q_INVOKABLE QVariantList optionsFor(const QString& key) const;

private:
    QVariantList icons_, cursors_;
};

} // namespace atrium
