#pragma once
// Plain holders for the shell's tree: `ShellRoot { Bar {} Dock {} }`. They
// own what is declared inside them and show nothing themselves.

#include <QObject>
#include <QQmlListProperty>
#include <QtQml/qqmlregistration.h>

namespace atrium::shell {

class Scope : public QObject {
    Q_OBJECT
    Q_PROPERTY(QQmlListProperty<QObject> data READ data)
    Q_CLASSINFO("DefaultProperty", "data")

public:
    using QObject::QObject;

    QQmlListProperty<QObject> data() {
        return {this, nullptr, &append, &count, &at, &clear};
    }

private:
    static void append(QQmlListProperty<QObject>* list, QObject* o) {
        auto* self = static_cast<Scope*>(list->object);
        if (!o)
            return;
        o->setParent(self);
        self->children_.append(o);
    }
    static qsizetype count(QQmlListProperty<QObject>* list) {
        return static_cast<Scope*>(list->object)->children_.size();
    }
    static QObject* at(QQmlListProperty<QObject>* list, qsizetype i) {
        return static_cast<Scope*>(list->object)->children_.at(i);
    }
    static void clear(QQmlListProperty<QObject>* list) {
        static_cast<Scope*>(list->object)->children_.clear();
    }

    QList<QObject*> children_;
};

// The root of a shell file.
class ShellRoot : public Scope {
    Q_OBJECT

public:
    using Scope::Scope;
};

// The root of a `pragma Singleton` file.
class Singleton : public Scope {
    Q_OBJECT

public:
    using Scope::Scope;
};

} // namespace atrium::shell
