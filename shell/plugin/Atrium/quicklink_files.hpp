#pragma once
// Import Quicklinks… and Export Quicklinks… in Settings > Launcher, as
// Tinycast has them (quicklink_archive_core.hpp: the file, and what's a
// duplicate). Export asks where through the file chooser portal, as an
// app's Save does.

#include <QObject>
#include <QUrl>
#include <QVariantMap>

namespace atrium {

class QuicklinkFiles : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    static QuicklinkFiles* instance();

    bool busy() const { return busy_; }

    // Adds what the file has that isn't here already (by name or link).
    Q_INVOKABLE void importFile(const QUrl& file);
    // All of them, to a file chosen in a save dialog.
    Q_INVOKABLE void exportAll();

signals:
    void busyChanged();
    // What happened, in a sentence; `failed` when nothing did.
    void finished(const QString& note, bool failed);

private slots:
    void saveChosen(uint response, const QVariantMap& results);

private:
    explicit QuicklinkFiles(QObject* parent = nullptr);
    void setBusy(bool busy);

    bool busy_ = false;
    QString request_;  // the portal request being answered
};

} // namespace atrium
