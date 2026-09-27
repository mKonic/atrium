#pragma once
// Exchange rates for the calculator: the ECB's daily reference rates
// (about thirty currencies against the euro), fetched at most twice a day
// when the palette is opened and cached in $XDG_CACHE_HOME/atrium, so sums
// in money work offline with yesterday's rates.

#include "calc.hpp"

#include <QDateTime>
#include <QObject>

#include <optional>

class QNetworkAccessManager;

namespace atrium {

class CurrencyRates : public QObject {
    Q_OBJECT

public:
    static CurrencyRates* instance();

    const calc::Rates* rates() const { return rates_ ? &*rates_ : nullptr; }
    // Fetch if the cached ones are old (or missing); quiet on failure.
    void refresh();

signals:
    void changed();

private:
    CurrencyRates();
    void load();

    std::optional<calc::Rates> rates_;
    QDateTime fetched_;
    bool fetching_ = false;
    QNetworkAccessManager* net_ = nullptr;
};

} // namespace atrium
