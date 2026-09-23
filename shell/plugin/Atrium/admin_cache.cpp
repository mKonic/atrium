#include "admin_cache.hpp"

namespace atrium {

AdminCache::AdminCache(QObject* parent) : QObject(parent) {
    expiry_.setSingleShot(true);
    expiry_.setInterval(kKeepMs);
    connect(&expiry_, &QTimer::timeout, this, &AdminCache::forget);
}

AdminCache::~AdminCache() {
    forget();
}

void AdminCache::stage(uint uid, const QString& password) {
    staged_.fill(QChar(0));
    staged_ = password;
    staged_.detach();  // our own copy, to overwrite later
    stagedUid_ = uid;
}

void AdminCache::commit() {
    if (staged_.isEmpty())
        return;
    password_.fill(QChar(0));
    password_ = staged_;
    password_.detach();
    uid_ = stagedUid_;
    staged_.fill(QChar(0));
    staged_.clear();
    expiry_.start();
}

QString AdminCache::recall(uint uid) const {
    return expiry_.isActive() && uid == uid_ ? password_ : QString();
}

void AdminCache::forget() {
    expiry_.stop();
    password_.fill(QChar(0));
    password_.clear();
    staged_.fill(QChar(0));
    staged_.clear();
    uid_ = stagedUid_ = 0;
}

} // namespace atrium
