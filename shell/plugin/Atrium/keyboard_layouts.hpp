#pragma once
// Keyboard layouts to pick from (xkeyboard-config's list), and the one in
// use: atrium's setting, else the system's.

#include <QObject>
#include <QVariantList>

namespace atrium {

class KeyboardLayouts : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList layouts READ layouts CONSTANT)  // [{value: "de", label: "German"}], by name
    Q_PROPERTY(QString current READ current NOTIFY currentChanged)

public:
    explicit KeyboardLayouts(QObject* parent = nullptr);

    QVariantList layouts() const { return layouts_; }
    QString current() const;

    // Just this layout, without a variant.
    Q_INVOKABLE void set(const QString& code);

signals:
    void currentChanged();

private:
    QVariantList layouts_;
};

} // namespace atrium
