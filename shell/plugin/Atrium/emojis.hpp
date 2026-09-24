#pragma once
// The emoji picker's model (Super+Period): every emoji the installed emoji
// font can draw, by group, searchable by name. Picking one types it into
// the field that had focus (or copies it, when there is none).

#include "emoji_core.hpp"

#include <QObject>
#include <QStringList>
#include <QVariantList>

namespace atrium {

class Emojis : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList groups READ groups CONSTANT)
    Q_PROPERTY(QString font READ font CONSTANT)  // the emoji font fontconfig picks

public:
    explicit Emojis(QObject* parent = nullptr);

    QStringList groups() const { return groups_; }
    QString font() const { return font_; }
    // [{text, name, group}]: all of them for an empty query.
    Q_INVOKABLE QVariantList find(const QString& query) const;
    Q_INVOKABLE void pick(const QString& text);
    // Where `group` starts among all of them (find("")), for its tab.
    Q_INVOKABLE int firstOf(const QString& group) const;

private:
    std::vector<emoji::Emoji> all_;
    QStringList groups_;
    QString font_;
};

} // namespace atrium
