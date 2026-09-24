#include "access.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <unistd.h>

#include <cstdio>

namespace atrium {

AccessPrompt::AccessPrompt(QObject* parent) : QObject(parent) {
    // The portal writes the question, then closes; a terminal would never end.
    if (isatty(STDIN_FILENO))
        return;
    QFile in;
    if (in.open(stdin, QIODevice::ReadOnly))
        q_ = QJsonDocument::fromJson(in.readAll()).object().toVariantMap();
    // Options as Dropdown takes them: {value, label}.
    for (const QVariant& c : q_.value("choices").toList()) {
        QVariantMap m = c.toMap();
        QVariantList options;
        for (const QVariant& o : m.value("options").toList())
            options.append(QVariantMap{{"value", o.toMap().value("id")}, {"label", o.toMap().value("label")}});
        m["options"] = options;
        choices_.append(m);
    }
}

QString AccessPrompt::grantLabel() const {
    const QString s = q_.value("grant").toString();
    return s.isEmpty() ? QStringLiteral("Allow") : s;
}

QString AccessPrompt::denyLabel() const {
    const QString s = q_.value("deny").toString();
    return s.isEmpty() ? QStringLiteral("Don't Allow") : s;
}

void AccessPrompt::setChoice(const QString& id, const QString& value) {
    for (QVariant& c : choices_) {
        QVariantMap m = c.toMap();
        if (m.value("id") == id) {
            m["value"] = value;
            c = m;
            emit choicesChanged();
            return;
        }
    }
}

void AccessPrompt::answer(bool allowed) {
    if (answered_)
        return;
    answered_ = true;
    QJsonObject picked;
    for (const QVariant& c : choices_) {
        const QVariantMap m = c.toMap();
        picked.insert(m.value("id").toString(), m.value("value").toString());
    }
    const QJsonObject reply{{"response", allowed ? 0 : 1}, {"choices", picked}};
    std::fputs(QJsonDocument(reply).toJson(QJsonDocument::Compact).append('\n').constData(), stdout);
    std::fflush(stdout);
    QCoreApplication::quit();
}

} // namespace atrium
