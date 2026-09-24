#include "keyed_model.hpp"

#include "list_sync.hpp"

#include <functional>

namespace atrium {

void KeyedModel::setValues(const QVariantList& v) {
    values_ = v;
    emit valuesChanged();
    sync();
}

void KeyedModel::setKey(const QString& k) {
    if (k == key_)
        return;
    key_ = k;
    emit keyChanged();
    sync();
}

void KeyedModel::setLimit(int n) {
    if (n == limit_)
        return;
    limit_ = n;
    emit limitChanged();
    sync();
}

void KeyedModel::sync() {
    std::vector<QVariant> next(values_.begin(), values_.end());
    if (limit_ > 0 && next.size() > size_t(limit_))
        next.resize(size_t(limit_));
    const auto keyOf = [this](const QVariant& v) {
        return key_.isEmpty() ? v : v.toMap().value(key_);
    };
    struct Ops {
        KeyedModel* m;
        void insert(int i, const std::function<void()>& f) {
            m->beginInsertRows({}, i, i);
            f();
            m->endInsertRows();
        }
        void move(int from, int to, const std::function<void()>& f) {
            m->beginMoveRows({}, from, from, {}, to > from ? to + 1 : to);
            f();
            m->endMoveRows();
        }
        void remove(int i, const std::function<void()>& f) {
            m->beginRemoveRows({}, i, i);
            f();
            m->endRemoveRows();
        }
        void change(int i, const std::function<void()>& f) {
            f();
            emit m->dataChanged(m->index(i), m->index(i));
        }
    } ops{this};
    sync_list(rows_, next, keyOf, ops);
}

int KeyedModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(rows_.size());
}

QVariant KeyedModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= int(rows_.size()) || role != Qt::UserRole)
        return {};
    return rows_[size_t(index.row())];
}

QHash<int, QByteArray> KeyedModel::roleNames() const {
    return {{Qt::UserRole, "modelData"}};
}

} // namespace atrium
