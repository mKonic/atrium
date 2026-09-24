#include "emojis.hpp"

#include "compositor.hpp"

#include <fontconfig/fontconfig.h>


namespace atrium {

namespace {

constexpr unsigned char kEmojiTest[] = {
#embed "../../../data/emoji/emoji-test.txt"
    , 0};

// The code points the system's emoji font covers: Unicode adds emoji every
// year and fonts follow later, and a picker full of boxes is no use.
FcCharSet* emoji_charset(QString& family) {
    FcPattern* want = FcNameParse(reinterpret_cast<const FcChar8*>("emoji"));
    FcConfigSubstitute(nullptr, want, FcMatchPattern);
    FcDefaultSubstitute(want);
    FcResult result;
    FcPattern* font = FcFontMatch(nullptr, want, &result);
    FcPatternDestroy(want);
    FcCharSet* set = nullptr;
    if (font) {
        FcCharSet* found = nullptr;
        if (FcPatternGetCharSet(font, FC_CHARSET, 0, &found) == FcResultMatch)
            set = FcCharSetCopy(found);
        FcChar8* name = nullptr;
        if (FcPatternGetString(font, FC_FAMILY, 0, &name) == FcResultMatch)
            family = QString::fromUtf8(reinterpret_cast<const char*>(name));
        FcPatternDestroy(font);
    }
    return set;
}

} // namespace

Emojis::Emojis(QObject* parent) : QObject(parent) {
    FcCharSet* covered = emoji_charset(font_);
    const std::string_view text(reinterpret_cast<const char*>(kEmojiTest), sizeof kEmojiTest - 1);
    for (emoji::Emoji& e : emoji::parse(text)) {
        if (covered && !FcCharSetHasChar(covered, FcChar32(e.first)))
            continue;
        const QString group = QString::fromStdString(e.group);
        if (!groups_.contains(group))
            groups_.append(group);
        all_.push_back(std::move(e));
    }
    if (covered)
        FcCharSetDestroy(covered);
}

QVariantList Emojis::find(const QString& query) const {
    QVariantList out;
    for (size_t i : emoji::search(all_, query.toStdString())) {
        const emoji::Emoji& e = all_[i];
        out.append(QVariantMap{{"text", QString::fromStdString(e.text)},
                               {"name", QString::fromStdString(e.name)},
                               {"group", QString::fromStdString(e.group)}});
    }
    return out;
}

int Emojis::firstOf(const QString& group) const {
    const std::string g = group.toStdString();
    for (size_t i = 0; i < all_.size(); ++i)
        if (all_[i].group == g)
            return int(i);
    return -1;
}

void Emojis::pick(const QString& text) {
    // Typed into the field the picker was opened over; when none takes it
    // atrium says so and the shell copies it (see Compositor::textNotInserted).
    Compositor::instance()->insertText(text);
}

} // namespace atrium
