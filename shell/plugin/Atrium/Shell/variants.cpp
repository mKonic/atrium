#include "variants.hpp"

#include <QJSValue>
#include <QQmlContext>
#include <QQmlEngine>

namespace atrium::shell {

namespace {

QVariantList entries(QVariant model) {
    if (model.metaType() == QMetaType::fromType<QJSValue>())
        model = model.value<QJSValue>().toVariant();
    QVariantList out;
    if (model.canConvert<QVariantList>())
        out = model.toList();
    else if (model.canConvert<int>()) {
        for (int i = 0; i < model.toInt(); ++i)
            out.append(i);
    }
    return out;
}

bool same(const QVariant& a, const QVariant& b) {
    QObject* oa = a.value<QObject*>();
    QObject* ob = b.value<QObject*>();
    if (oa || ob)
        return oa == ob;
    return a == b;
}

} // namespace

Variants::~Variants() {
    for (Instance& i : instances_)
        delete i.object;
}

void Variants::setModel(const QVariant& model) {
    model_ = model;
    emit modelChanged();
    update();
}

void Variants::setDelegate(QQmlComponent* delegate) {
    if (delegate_ == delegate)
        return;
    delegate_ = delegate;
    for (Instance& i : instances_)
        delete i.object;
    instances_.clear();
    emit delegateChanged();
    update();
}

QList<QObject*> Variants::instances() const {
    QList<QObject*> out;
    for (const Instance& i : instances_)
        if (i.object)
            out.append(i.object);
    return out;
}

void Variants::componentComplete() {
    complete_ = true;
    update();
}

void Variants::update() {
    if (!complete_ || !delegate_)
        return;
    const QVariantList values = entries(model_);
    bool changed = false;

    // Gone from the model: their instances go.
    for (qsizetype i = instances_.size() - 1; i >= 0; --i) {
        const bool kept = std::any_of(values.begin(), values.end(),
                                      [&](const QVariant& v) { return same(v, instances_[i].value); });
        if (!kept) {
            delete instances_[i].object;
            instances_.removeAt(i);
            changed = true;
        }
    }
    // New in the model: an instance each, in model order.
    QList<Instance> ordered;
    for (const QVariant& v : values) {
        auto it = std::find_if(instances_.begin(), instances_.end(),
                               [&](const Instance& i) { return same(v, i.value); });
        if (it != instances_.end()) {
            ordered.append(*it);
            continue;
        }
        ordered.append({v, create(v)});
        changed = true;
    }
    instances_ = ordered;
    if (changed)
        emit instancesChanged();
}

QObject* Variants::create(const QVariant& value) {
    QQmlContext* context = delegate_->creationContext() ? delegate_->creationContext() : qmlContext(this);
    QObject* o = delegate_->createWithInitialProperties({{"modelData", value}}, context);
    if (!o) {
        qWarning().noquote() << "Variants:" << delegate_->errorString();
        return nullptr;
    }
    o->setParent(this);
    return o;
}

} // namespace atrium::shell
