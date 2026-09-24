#pragma once
// The welcome shown the first time someone logs in: `Welcome.due` until
// `done()`, remembered in the user's state directory.

#include <QObject>

namespace atrium {

class Welcome : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool due READ due NOTIFY dueChanged)

public:
    explicit Welcome(QObject* parent = nullptr);

    bool due() const { return due_; }
    Q_INVOKABLE void done();

signals:
    void dueChanged();

private:
    QString marker_;
    bool due_;
};

} // namespace atrium
