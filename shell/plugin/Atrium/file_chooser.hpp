#pragma once
// The file chooser (files.qml), run by atrium-portal's FileChooser when an
// app asks to open or save: it reads the request from stdin and answers on
// stdout, as JSON. What the buttons, Return and the name field do follows
// GTK 3's GtkFileChooserWidget (chooser_core.hpp).
//
//   asked:    { mode: "open"|"save"|"saveFiles", app, title, accept, multiple,
//               directory, filters: [{ name, patterns: [{ glob }|{ mime }] }],
//               filter, choices: [{ id, label, options: [{ id, label }], value }],
//               folder, name, file, files: [names] }
//   answered: { uris: [file:// URLs], choices: [[id, value]], filter, writable }
//             (nothing when cancelled)

#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace atrium {

class FileChooser : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode CONSTANT)
    // What the list is for: "open", "save" or "folder" (saveFiles chooses a folder).
    Q_PROPERTY(QString action READ action CONSTANT)
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QString acceptLabel READ acceptLabel CONSTANT)
    Q_PROPERTY(bool multiple READ multiple CONSTANT)
    Q_PROPERTY(bool directory READ directory CONSTANT)
    // Whether the app said where to start (else Recent, as GTK starts).
    Q_PROPERTY(bool startsSomewhere READ startsSomewhere CONSTANT)
    Q_PROPERTY(QStringList filters READ filters CONSTANT)
    Q_PROPERTY(int filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(QVariantList patterns READ patterns NOTIFY filterChanged)
    Q_PROPERTY(QVariantList choices READ choices NOTIFY choicesChanged)
    // Opening: "Open files read-only" (the answer's writable is its opposite).
    Q_PROPERTY(bool readOnly READ readOnly WRITE setReadOnly NOTIFY readOnlyChanged)
    Q_PROPERTY(QString folder READ folder CONSTANT)
    Q_PROPERTY(QString name READ name CONSTANT)
    // Saving over a file: the one to pick in the list.
    Q_PROPERTY(QString file READ file CONSTANT)
    Q_PROPERTY(QStringList files READ files CONSTANT)

public:
    explicit FileChooser(QObject* parent = nullptr);
    // As if read from stdin (for tests).
    explicit FileChooser(const QByteArray& request, QObject* parent = nullptr);

    QString mode() const { return mode_; }
    QString action() const;
    QString title() const;
    QString acceptLabel() const;
    bool multiple() const { return multiple_; }
    bool directory() const { return directory_; }
    bool startsSomewhere() const { return startsSomewhere_; }
    QStringList filters() const;
    int filter() const { return filter_; }
    void setFilter(int index);
    QVariantList patterns() const;
    QVariantList choices() const { return choices_; }
    bool readOnly() const { return readOnly_; }
    void setReadOnly(bool on);
    QString folder() const { return folder_; }
    QString name() const { return name_; }
    QString file() const { return file_; }
    QStringList files() const { return files_; }

    Q_INVOKABLE void setChoice(const QString& id, const QString& value);
    // GTK's check_valid_child_name: { error, warning } for a new name in `folder`.
    Q_INVOKABLE QVariantMap checkName(const QString& folder, const QString& name, bool isFolder) const;

    // The accept button (or Return) with the list in charge, `picked` in
    // `folder`. What the dialog does next:
    //   { enter: path }       go into that folder
    //   { entry: true }       the name field decides: call acceptText
    //   {}                    answered (the dialog ends), or nothing to do
    Q_INVOKABLE QVariantMap acceptList(const QStringList& picked, const QString& folder);
    // The same with typed text in charge (the save name, or the location
    // field), against `folder`:
    //   { enter: path, clear }  go into that folder
    //   { error, detail }       say what's wrong
    //   { confirm, detail }     ask before replacing: again with replace
    //   { focus: true }         nothing typed: back to the field
    //   { list: true }          nothing typed: the list decides
    //   {}                      answered
    Q_INVOKABLE QVariantMap acceptText(const QString& text, const QString& folder, bool replace = false);
    // A new folder: { path } or { error }.
    Q_INVOKABLE QVariantMap makeFolder(const QString& folder, const QString& name) const;
    // Rename, Move to Trash, Delete (the list's menu, saving only): {} or { error }.
    Q_INVOKABLE QVariantMap rename(const QString& path, const QString& name) const;
    Q_INVOKABLE QVariantMap trash(const QString& path) const;
    Q_INVOKABLE QVariantMap remove(const QString& path) const;
    Q_INVOKABLE void copyLocations(const QStringList& paths) const;
    Q_INVOKABLE void openInFileManager(const QString& path) const;

    // The answers; each ends the dialog.
    Q_INVOKABLE void open(const QStringList& paths);
    Q_INVOKABLE void save(const QString& path);
    Q_INVOKABLE void saveFiles(const QString& folder);
    Q_INVOKABLE void cancel();

    QStringList saveFilesPaths(const QString& folder) const;
    QByteArray answer() const { return answer_; }
    // Where answers are recorded as recently used (tests point it elsewhere).
    void setRecentFile(const QString& path) { recentFile_ = path; }

signals:
    void filterChanged();
    void choicesChanged();
    void readOnlyChanged();

private:
    void read(const QByteArray& request);
    void answer(const QStringList& paths);
    void addRecent(const QStringList& paths) const;

    QString mode_ = "open", app_, title_, accept_, folder_, name_, file_, recentFile_;
    bool multiple_ = false, directory_ = false, readOnly_ = false, startsSomewhere_ = false;
    QVariantList filters_, choices_;
    int filter_ = -1;
    QStringList files_;
    QByteArray answer_;
    bool answered_ = false, quit_ = true;
};

} // namespace atrium
