#pragma once
// One instance of `delegate` per model entry, each told its entry through a
// `modelData` property: `Variants { model: Shell.screens; Bar {} }`. An entry
// that stays keeps its instance; only added and removed ones change.

#include <QObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlParserStatus>
#include <QVariant>

namespace atrium::shell {

class Variants : public QObject, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    Q_PROPERTY(QVariant model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(QQmlComponent* delegate READ delegate WRITE setDelegate NOTIFY delegateChanged)
    Q_PROPERTY(QList<QObject*> instances READ instances NOTIFY instancesChanged)
    Q_CLASSINFO("DefaultProperty", "delegate")

public:
    using QObject::QObject;
    ~Variants() override;

    QVariant model() const { return model_; }
    void setModel(const QVariant& model);
    QQmlComponent* delegate() const { return delegate_; }
    void setDelegate(QQmlComponent* delegate);
    QList<QObject*> instances() const;

    void classBegin() override {}
    void componentComplete() override;

signals:
    void modelChanged();
    void delegateChanged();
    void instancesChanged();

private:
    struct Instance {
        QVariant value;
        QPointer<QObject> object;
    };
    void update();
    QObject* create(const QVariant& value);

    QVariant model_;
    QPointer<QQmlComponent> delegate_;
    QList<Instance> instances_;
    bool complete_ = false;
};

} // namespace atrium::shell
