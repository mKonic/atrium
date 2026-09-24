#pragma once
// A list model over plain values that keeps each item's delegate while the
// item stays: `KeyedModel { values: windows; key: "id"; limit: 4 }`.
// Items are told apart by `key`; new ones are inserted, gone ones removed,
// moved ones moved, and changed ones updated in place (their delegates'
// modelData follows).

#include <QAbstractListModel>
#include <QVariant>

#include <vector>

namespace atrium {

class KeyedModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QVariantList values READ values WRITE setValues NOTIFY valuesChanged)
    Q_PROPERTY(QString key READ key WRITE setKey NOTIFY keyChanged)
    Q_PROPERTY(int limit READ limit WRITE setLimit NOTIFY limitChanged)  // 0: all

public:
    using QAbstractListModel::QAbstractListModel;

    QVariantList values() const { return values_; }
    void setValues(const QVariantList& v);
    QString key() const { return key_; }
    void setKey(const QString& k);
    int limit() const { return limit_; }
    void setLimit(int n);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void valuesChanged();
    void keyChanged();
    void limitChanged();

private:
    void sync();

    QVariantList values_;
    QString key_;
    int limit_ = 0;
    std::vector<QVariant> rows_;
};

} // namespace atrium
