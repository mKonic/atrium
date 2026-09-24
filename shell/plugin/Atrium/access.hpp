#pragma once
// The portal's permission question (access.qml), read from stdin as JSON:
// {app, title, subtitle, body, icon, grant, deny, choices: [{id, label,
// options: [{id, label}], value}]}. `answer(allowed)` writes
// {response, choices: {id: value}} to stdout and quits.

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

namespace atrium {

class AccessPrompt : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString app READ app CONSTANT)
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QString subtitle READ subtitle CONSTANT)
    Q_PROPERTY(QString body READ body CONSTANT)
    Q_PROPERTY(QString icon READ icon CONSTANT)
    Q_PROPERTY(QString grantLabel READ grantLabel CONSTANT)
    Q_PROPERTY(QString denyLabel READ denyLabel CONSTANT)
    // [{id, label, options: [{value, label}], value}]; no options: a switch.
    Q_PROPERTY(QVariantList choices READ choices NOTIFY choicesChanged)

public:
    explicit AccessPrompt(QObject* parent = nullptr);

    QString app() const { return q_.value("app").toString(); }
    QString title() const { return q_.value("title").toString(); }
    QString subtitle() const { return q_.value("subtitle").toString(); }
    QString body() const { return q_.value("body").toString(); }
    QString icon() const { return q_.value("icon").toString(); }
    QString grantLabel() const;
    QString denyLabel() const;
    QVariantList choices() const { return choices_; }

    Q_INVOKABLE void setChoice(const QString& id, const QString& value);
    Q_INVOKABLE void answer(bool allowed);

signals:
    void choicesChanged();

private:
    QVariantMap q_;
    QVariantList choices_;
    bool answered_ = false;
};

} // namespace atrium
