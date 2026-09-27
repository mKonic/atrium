#include "launcher_ranking.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <cmath>

namespace atrium {

namespace {

QString stateDir() {
    QString dir = qEnvironmentVariable("XDG_STATE_HOME");
    if (dir.isEmpty())
        dir = QDir::homePath() + "/.local/state";
    return dir + "/atrium";
}

} // namespace

rank::Fold qtFold() {
    return [](char32_t c) -> std::u32string {
        if (c < 0x80) {
            if (c >= U'A' && c <= U'Z')
                c += U'a' - U'A';
            return std::u32string(1, c);
        }
        // Decompose, drop the marks, fold case: "É" → "e", full-width → ASCII.
        const QString s = QString::fromUcs4(&c, 1).normalized(QString::NormalizationForm_KD);
        QString out;
        for (const QChar ch : s)
            if (ch.category() != QChar::Mark_NonSpacing && ch.category() != QChar::Mark_Enclosing &&
                ch.category() != QChar::Mark_SpacingCombining)
                out += ch;
        out = out.toCaseFolded();
        const std::u32string r = out.toStdU32String();
        return r.empty() ? std::u32string(1, c) : r;
    };
}

rank::Text prepareText(const QString& text) {
    static const rank::Fold fold = qtFold();
    return rank::prepare(text.toStdU32String(), fold);
}

std::u32string foldText(const QString& text) {
    static const rank::Fold fold = qtFold();
    return rank::fold_all(text.trimmed().toStdU32String(), fold);
}

RankingStore::RankingStore(QString file) : file_(file.isEmpty() ? stateDir() + "/launcher-ranking.json" : file) {
    load();
    if (!QFileInfo::exists(file_))
        importCounts();
}

double RankingStore::now() {
    return double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

const rank::Visit* RankingStore::visit(const QString& key) const {
    auto it = visits_.constFind(key);
    return it == visits_.constEnd() ? nullptr : &*it;
}

void RankingStore::record(const QString& key, const QString& query) {
    rank::record_visit(visits_[key], now(), foldText(query));
    ++revision_;
    save();
}

void RankingStore::reset(const QString& key) {
    if (visits_.remove(key)) {
        ++revision_;
        save();
    }
}

void RankingStore::resetAll() {
    visits_.clear();
    ++revision_;
    save();
}

void RankingStore::load() {
    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject all = QJsonDocument::fromJson(f.readAll()).object();
    const double t = now();
    for (auto it = all.begin(); it != all.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        rank::Visit v;
        v.anchor = o.value("anchor").toDouble();
        v.last = o.value("last").toDouble();
        for (const QJsonValue& term : o.value("terms").toArray())
            v.terms.push_back(term.toString().toStdU32String());
        if (!rank::expired(v, t))  // scores as never opened: pruned
            visits_.insert(it.key(), std::move(v));
    }
}

void RankingStore::save() const {
    QJsonObject all;
    for (auto it = visits_.begin(); it != visits_.end(); ++it) {
        QJsonArray terms;
        for (const std::u32string& term : it->terms)
            terms.append(QString::fromStdU32String(term));
        all.insert(it.key(), QJsonObject{{"anchor", it->anchor}, {"last", it->last}, {"terms", terms}});
    }
    QDir().mkpath(QFileInfo(file_).path());
    QSaveFile f(file_);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(all).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

void RankingStore::importCounts() {
    QFile f(QFileInfo(file_).path() + "/launcher.json");
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject counts = QJsonDocument::fromJson(f.readAll()).object();
    const double t = now();
    for (auto it = counts.begin(); it != counts.end(); ++it) {
        rank::Visit& v = visits_["app:" + it.key()];
        // A few visits' worth, today: enough to lead, not to stick forever.
        for (int i = 0; i < std::min(it.value().toInt(), 3); ++i)
            rank::record_visit(v, t, {});
    }
    if (!visits_.isEmpty())
        save();
}

} // namespace atrium
