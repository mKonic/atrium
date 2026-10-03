#pragma once
// "Open With" (openwith.qml), run by atrium-portal's AppChooser when an app
// asks which app should open a file or link. The request is a line of JSON
// on stdin; more lines bring a new list of apps while it's open (one was
// installed). The app picked is answered on stdout.
//
//   asked:    { choices: [desktop ids], last, contentType, filename, uri }
//   then:     { choices: [desktop ids] }
//   answered: { choice: id }   (nothing when cancelled)

#include <QObject>
#include <QSocketNotifier>
#include <QStringList>
#include <QVariantList>

namespace atrium {

class AppChooser : public QObject {
    Q_OBJECT
    // Desktop ids ("org.kde.okular"), the last one used first.
    Q_PROPERTY(QStringList choices READ choices NOTIFY choicesChanged)
    // The same as [{ id, name, icon, detail }] from their desktop files;
    // detail is the id when two go by one name.
    Q_PROPERTY(QVariantList apps READ apps NOTIFY choicesChanged)
    Q_PROPERTY(QString last READ last NOTIFY choicesChanged)
    // What's opened: the file's name (or the link), and its kind ("PDF document").
    Q_PROPERTY(QString subject READ subject NOTIFY choicesChanged)
    Q_PROPERTY(QString kind READ kind NOTIFY choicesChanged)

public:
    explicit AppChooser(QObject* parent = nullptr);  // reads stdin
    AppChooser(bool, QObject* parent);               // reads nothing (tests: feed)

    QStringList choices() const { return choices_; }
    QVariantList apps() const { return apps_; }
    QString last() const { return last_; }
    QString subject() const { return subject_; }
    QString kind() const { return kind_; }

    // One line of what stdin brings.
    void feed(const QByteArray& line);

    Q_INVOKABLE void choose(const QString& id);
    Q_INVOKABLE void cancel();

    QByteArray answer() const { return answer_; }  // the last answer (tests)

signals:
    void choicesChanged();

private:
    void readInput();
    void finish();

    QStringList choices_;
    QVariantList apps_;
    QString last_, subject_, kind_;
    QByteArray pending_, answer_;
    QSocketNotifier* input_ = nullptr;
    bool answered_ = false, quit_ = true;
};

} // namespace atrium
