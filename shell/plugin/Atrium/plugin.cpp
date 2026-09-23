// The Atrium QML module: C++ helpers for atrium's shell, so its QML stays
// layout and bindings. `import Atrium` in the shell.

#include "compositor.hpp"
#include "views.hpp"
#include "search.hpp"

#include <QObject>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <vector>

namespace atrium {

// Launcher search, exposed as the `Search` singleton.
class SearchApi : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // 0 is no match; see search::score.
    Q_INVOKABLE int score(const QString& query, const QString& text) const {
        return search::score(query.toLower().toStdString(), text.toLower().toStdString());
    }

    // The best score over several texts, each weighted (weights in percent).
    Q_INVOKABLE int bestScore(const QString& query, const QStringList& texts, const QList<int>& weights) const {
        const std::string q = query.toLower().toStdString();
        int best = 0;
        for (qsizetype i = 0; i < texts.size(); ++i) {
            const int w = i < weights.size() ? weights[i] : 100;
            best = std::max(best, search::score(q, texts[i].toLower().toStdString()) * w / 100);
        }
        return best;
    }

    // Apps (Quickshell DesktopEntry objects) best first for `query`, launch
    // counts breaking ties and lifting favourites; with no query, the most
    // used. Hidden entries never show.
    Q_INVOKABLE QVariantList rankApps(const QString& query, const QVariantList& entries,
                                      const QVariantMap& counts, int limit) const {
        const std::string q = query.trimmed().toLower().toStdString();
        struct Ranked {
            double score;
            QVariant entry;
        };
        std::vector<Ranked> ranked;
        for (const QVariant& v : entries) {
            QObject* e = v.value<QObject*>();
            if (!e || e->property("noDisplay").toBool())
                continue;
            const int uses = counts.value(e->property("id").toString()).toInt();
            double s;
            if (q.empty()) {
                s = uses;
            } else {
                const auto field = [&](const char* name) {
                    const QVariant p = e->property(name);
                    return (p.canConvert<QStringList>() && p.typeId() != QMetaType::QString
                                ? p.toStringList().join(' ') : p.toString()).toLower().toStdString();
                };
                s = std::max({search::score(q, field("name")) * 1.0, search::score(q, field("genericName")) * 0.8,
                              search::score(q, field("keywords")) * 0.7, search::score(q, field("id")) * 0.6});
                // Loose matches in a description or keyword list are noise.
                if (s < 150)
                    continue;
                s += std::log(uses + 1.0) * 40;
            }
            if (s > 0)
                ranked.push_back({s, v});
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.score > b.score; });
        QVariantList out;
        for (const Ranked& r : ranked) {
            if (int(out.size()) >= limit)
                break;
            out.push_back(r.entry);
        }
        return out;
    }

    // The result of a calculation as text, or "" when `text` is not one.
    Q_INVOKABLE QString calculate(const QString& text) const {
        const auto v = search::calculate(text.toStdString());
        return v ? QString::fromStdString(search::format_number(*v)) : QString();
    }
};

class AtriumPlugin : public QQmlExtensionPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)

public:
    void registerTypes(const char* uri) override {
        qmlRegisterSingletonType<SearchApi>(uri, 1, 0, "Search", [](QQmlEngine*, QJSEngine*) -> QObject* {
            return new SearchApi;
        });
        // The compositor, as live state: `Atrium.windows`, `Atrium.switchSpace(2)`.
        qmlRegisterSingletonType<Compositor>(uri, 1, 0, "Atrium", [](QQmlEngine*, QJSEngine*) -> QObject* {
            QObject* c = Compositor::instance();
            QQmlEngine::setObjectOwnership(c, QQmlEngine::CppOwnership);
            return c;
        });
        qmlRegisterType<OutputState>(uri, 1, 0, "OutputState");
        qmlRegisterType<SpaceWindows>(uri, 1, 0, "SpaceWindows");
    }
};

} // namespace atrium

#include "plugin.moc"
