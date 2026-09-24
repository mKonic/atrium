#pragma once
// The system's language and formats (dates, numbers, money, measures),
// through systemd's localed: what apps get as LANG and LC_* from the next
// login on. Only locales generated on this system can be picked.
// `LocaleSettings` in QML, since Atrium.Shell already has a Region.

#include <QObject>
#include <QVariantList>

namespace atrium {

class Region : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString language READ language NOTIFY changed)  // "de_DE.UTF-8"
    Q_PROPERTY(QString formats READ formats NOTIFY changed)    // LC_TIME's, else the language's
    Q_PROPERTY(QVariantList locales READ locales CONSTANT)     // [{value, label}], by label
    // How the formats look: "Thursday 24 September 2026 · 14:05 · 1,234,567.89 · €12.50".
    Q_PROPERTY(QString sample READ sample NOTIFY changed)

public:
    explicit Region(QObject* parent = nullptr);

    bool available() const { return available_; }
    QString language() const { return value("LANG"); }
    QString formats() const;
    QVariantList locales() const;
    QString sample() const;

    Q_INVOKABLE void setLanguage(const QString& locale);
    Q_INVOKABLE void setFormats(const QString& locale);

    static QString label(const QString& locale);

signals:
    void changed();
    void failed(const QString& why);

private slots:
    void propertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);

private:
    void load();
    QString value(const QString& key) const;
    void setLocale(const QStringList& assignments);

    bool available_ = false;
    QStringList locale_;  // localed's "Locale": ["LANG=…", "LC_TIME=…"]
    mutable QVariantList locales_;
};

} // namespace atrium
