#pragma once
// The file chooser (files.qml), run by atrium-portal's FileChooser when an
// app asks to open or save: it reads the request from stdin and answers on
// stdout, as JSON.
//
//   asked:    { mode: "open"|"save"|"saveFiles", app, title, accept, multiple,
//               directory, filters: [{ name, patterns: [{ glob }|{ mime }] }],
//               filter, choices: [{ id, label, options: [{ id, label }], value }],
//               folder, name, files: [names] }
//   answered: { uris: [file:// URLs], choices: [[id, value]], filter }
//             (nothing when cancelled)

#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace atrium {

class FileChooser : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode CONSTANT)
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QString acceptLabel READ acceptLabel CONSTANT)
    Q_PROPERTY(bool multiple READ multiple CONSTANT)
    Q_PROPERTY(bool directory READ directory CONSTANT)
    // The filters' names, and the one on (-1: none offered).
    Q_PROPERTY(QStringList filters READ filters CONSTANT)
    Q_PROPERTY(int filter READ filter WRITE setFilter NOTIFY filterChanged)
    // The patterns of the filter on, for a FolderModel.
    Q_PROPERTY(QVariantList patterns READ patterns NOTIFY filterChanged)
    // The app's extra choices: [{ id, label, options: [{ id, label }], value }]
    // (no options: a checkbox, "true" or "false").
    Q_PROPERTY(QVariantList choices READ choices NOTIFY choicesChanged)
    Q_PROPERTY(QString folder READ folder CONSTANT)  // where it starts
    Q_PROPERTY(QString name READ name CONSTANT)      // the name to save under
    Q_PROPERTY(QStringList files READ files CONSTANT)  // saveFiles: their names

public:
    explicit FileChooser(QObject* parent = nullptr);
    // As if read from stdin (for tests).
    explicit FileChooser(const QByteArray& request, QObject* parent = nullptr);

    QString mode() const { return mode_; }
    QString title() const;
    QString acceptLabel() const;
    bool multiple() const { return multiple_; }
    bool directory() const { return directory_; }
    QStringList filters() const;
    int filter() const { return filter_; }
    void setFilter(int index);
    QVariantList patterns() const;
    QVariantList choices() const { return choices_; }
    QString folder() const { return folder_; }
    QString name() const { return name_; }
    QStringList files() const { return files_; }

    Q_INVOKABLE void setChoice(const QString& id, const QString& value);
    // What's wrong with saving `name` in `folder` ("" when nothing is), and
    // whether it would replace a file there.
    Q_INVOKABLE QString saveProblem(const QString& folder, const QString& name) const;
    Q_INVOKABLE bool exists(const QString& folder, const QString& name) const;

    // The accept button (or Return, or a double-click) with these picked in
    // `folder`, and the name typed when saving. What the dialog does next:
    //   { enter: path }    go into that folder
    //   { error: text }    say what's wrong
    //   { confirm: text }  ask first (replacing a file): again with replace
    //   {}                 answered, the dialog ends
    Q_INVOKABLE QVariantMap accept(const QStringList& picked, const QString& folder, const QString& name,
                                   bool replace = false);
    // A new folder `name` in `folder`: { path } or { error }.
    Q_INVOKABLE QVariantMap makeFolder(const QString& folder, const QString& name) const;

    // The answers; each ends the dialog.
    Q_INVOKABLE void open(const QStringList& paths);
    Q_INVOKABLE void save(const QString& folder, const QString& name);
    Q_INVOKABLE void saveFiles(const QString& folder);  // each under a free name
    Q_INVOKABLE void cancel();

    // What `saveFiles` answers, without answering (for tests).
    QStringList saveFilesPaths(const QString& folder) const;
    // The last answer written (for tests).
    QByteArray answer() const { return answer_; }

signals:
    void filterChanged();
    void choicesChanged();

private:
    void read(const QByteArray& request);
    void answer(const QStringList& paths);

    QString mode_ = "open", app_, title_, accept_, folder_, name_;
    bool multiple_ = false, directory_ = false;
    QVariantList filters_, choices_;
    int filter_ = -1;
    QStringList files_;
    QByteArray answer_;
    bool answered_ = false, quit_ = true;
};

} // namespace atrium
