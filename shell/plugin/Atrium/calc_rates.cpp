#include "calc_rates.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>

namespace atrium {

namespace {

constexpr int kStaleHours = 12;
const char* const kUrl = "https://www.ecb.europa.eu/stats/eurofxref/eurofxref-daily.xml";

QString cacheFile() {
    QString dir = qEnvironmentVariable("XDG_CACHE_HOME");
    if (dir.isEmpty())
        dir = QDir::homePath() + "/.cache";
    return dir + "/atrium/ecb-rates.xml";
}

} // namespace

CurrencyRates* CurrencyRates::instance() {
    static auto* self = new CurrencyRates;
    return self;
}

CurrencyRates::CurrencyRates() {
    load();
}

void CurrencyRates::load() {
    QFile f(cacheFile());
    if (!f.open(QIODevice::ReadOnly))
        return;
    rates_ = calc::parse_ecb(f.readAll().toStdString());
    fetched_ = QFileInfo(f).lastModified();
}

void CurrencyRates::refresh() {
    if (fetching_ || (rates_ && fetched_.secsTo(QDateTime::currentDateTime()) < kStaleHours * 3600))
        return;
    if (!net_)
        net_ = new QNetworkAccessManager(this);
    fetching_ = true;
    QNetworkRequest req{QUrl(kUrl)};
    req.setTransferTimeout(10000);
    QNetworkReply* reply = net_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        fetching_ = false;
        if (reply->error() != QNetworkReply::NoError)
            return;
        const QByteArray body = reply->readAll();
        auto parsed = calc::parse_ecb(body.toStdString());
        if (!parsed)
            return;
        rates_ = std::move(parsed);
        fetched_ = QDateTime::currentDateTime();
        QDir().mkpath(QFileInfo(cacheFile()).path());
        QSaveFile out(cacheFile());
        if (out.open(QIODevice::WriteOnly)) {
            out.write(body);
            out.commit();
        }
        emit changed();
    });
}

} // namespace atrium
