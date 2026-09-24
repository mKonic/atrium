#pragma once
// Keyboard layouts to pick from (xkeyboard-config's list), and the one in
// use: atrium's setting, else the system's.

#include <QHash>
#include <QObject>
#include <QVariantList>

namespace atrium {

class KeyboardLayouts : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList layouts READ layouts CONSTANT)  // [{value: "de", label: "German"}], by name
    Q_PROPERTY(QString current READ current NOTIFY currentChanged)
    // The layouts in use, in order: [{layout, variant, label}].
    Q_PROPERTY(QVariantList sources READ sources NOTIFY currentChanged)

public:
    explicit KeyboardLayouts(QObject* parent = nullptr);

    QVariantList layouts() const { return layouts_; }
    QString current() const;

    QVariantList sources() const;

    // Just this layout, without a variant.
    Q_INVOKABLE void set(const QString& code);
    // Layouts and variants whose name has `query` in it: [{layout, variant, label}].
    Q_INVOKABLE QVariantList find(const QString& query) const;
    Q_INVOKABLE void add(const QString& layout, const QString& variant);
    Q_INVOKABLE void remove(int index);
    Q_INVOKABLE void moveUp(int index);  // the first is what new windows start with

signals:
    void currentChanged();

private:
    struct Source {
        QString layout, variant;
    };
    QList<Source> read() const;
    void write(const QList<Source>& sources);
    QString label(const Source& s) const;

    QVariantList layouts_;
    QVariantList all_;  // layouts and variants, by name
    QHash<QString, QString> names_;  // "de" and "de(nodeadkeys)" → name
};

} // namespace atrium
