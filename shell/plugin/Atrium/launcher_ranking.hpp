#pragma once
// The palette's learned ranking on disk ($XDG_STATE_HOME/atrium/launcher-ranking.json):
// one rank::Visit per entry key. Machine-local learned data, not a setting,
// so it lives beside the other state files rather than in the registry.
// Also the Qt side of rank.hpp: text folded for matching.

#include "rank.hpp"

#include <QHash>
#include <QString>

namespace atrium {

// Case and accents folded, as rank::Text / rank::Fields want it.
rank::Fold qtFold();
rank::Text prepareText(const QString& text);
std::u32string foldText(const QString& text);

class RankingStore {
public:
    // `file` empty: the default path.
    explicit RankingStore(QString file = {});

    const rank::Visit* visit(const QString& key) const;
    // Opened now, found by `query` as typed ("" when not by typing).
    void record(const QString& key, const QString& query);
    void reset(const QString& key);
    void resetAll();
    bool learned(const QString& key) const { return visits_.contains(key); }
    // Bumped by every change, for caches keyed on it.
    int revision() const { return revision_; }

    static double now();

private:
    void load();
    void save() const;
    // The Spotlight launcher's use counts (launcher.json), once.
    void importCounts();

    QString file_;
    QHash<QString, rank::Visit> visits_;
    int revision_ = 0;
};

} // namespace atrium
