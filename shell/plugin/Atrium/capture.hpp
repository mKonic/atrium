#pragma once
// Screenshots (capture.qml, its own process). Every screen is frozen as it
// was when it started (grim), and the picking happens over those pictures:
// a region, a window (grim takes it whole, even behind others) or a screen.
// The shot goes to ~/Pictures/Screenshots and the clipboard. Started by the
// Screenshot portal it answers on stdout instead: the file's URI, or a
// picked colour.
//
//   ATRIUM_CAPTURE_MODE: toolbar (default), region, window, screen, portal,
//   portal-interactive, color

#include <QHash>
#include <QImage>
#include <QObject>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>

namespace atrium {

class Capture : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode CONSTANT)
    Q_PROPERTY(bool forPortal READ forPortal CONSTANT)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)          // every screen frozen
    Q_PROPERTY(QVariantMap frozen READ frozen NOTIFY readyChanged)  // screen name → picture
    // What a click or drag takes: region, window, screen, color.
    Q_PROPERTY(QString kind READ kind WRITE setKind NOTIFY kindChanged)
    // The Print toolbar (screen, window or region, and recording) is shown.
    Q_PROPERTY(bool toolbar READ toolbar CONSTANT)
    // The screens are covered for picking: frozen, and nothing taken yet.
    Q_PROPERTY(bool picking READ picking NOTIFY pickingChanged)
    Q_PROPERTY(QString lastFile READ lastFile NOTIFY taken)
    Q_PROPERTY(QUrl lastUrl READ lastUrl NOTIFY taken)

public:
    explicit Capture(QObject* parent = nullptr);

    QString mode() const { return mode_; }
    bool forPortal() const { return mode_.startsWith("portal") || mode_ == "color"; }
    bool ready() const { return ready_; }
    QVariantMap frozen() const;
    QString kind() const { return kind_; }
    void setKind(const QString& kind);
    bool toolbar() const { return mode_ == "toolbar" || mode_ == "portal-interactive"; }
    bool picking() const { return ready_ && !busy_ && last_.isEmpty() && !done_ && mode_ != "screen" && mode_ != "portal"; }
    QString lastFile() const { return last_; }
    QUrl lastUrl() const { return QUrl::fromLocalFile(last_); }

    // The window under (x, y) on `screen` (its own coordinates), top first:
    // {identifier, title, appId, x, y, width, height} of what a shot of it
    // holds (below the title bar), on that screen; empty for none.
    Q_INVOKABLE QVariantMap windowAt(const QString& screen, int x, int y) const;
    // "#rrggbb" of the frozen picture there.
    Q_INVOKABLE QString colorAt(const QString& screen, int x, int y) const;

    Q_INVOKABLE void takeRegion(const QString& screen, int x0, int y0, int x1, int y1);
    Q_INVOKABLE void takeScreen(const QString& screen);  // "" all of them, side by side
    // The window by itself; where the app's buffer can't be copied (a format
    // grim doesn't read), cut from the frozen screen at x, y, width, height.
    Q_INVOKABLE void takeWindow(const QString& identifier, const QString& screen = {}, int x = 0, int y = 0,
                                int width = 0, int height = 0);
    Q_INVOKABLE void pickColor(const QString& screen, int x, int y);
    Q_INVOKABLE void cancel();
    // Screen recording, which the desktop shell does.
    Q_INVOKABLE void record();
    Q_INVOKABLE void deleteLast();

signals:
    void readyChanged();
    void kindChanged();
    void pickingChanged();
    void taken();
    void failed(const QString& why);

private:
    void freeze();
    void pickingMayChange();
    void deliver(const QImage& image);
    void answer(const QString& line);  // the portal's; then quit

    QString mode_, kind_;
    bool ready_ = false;
    bool busy_ = false;  // grim taking a window
    bool done_ = false;
    QTemporaryDir dir_;
    QHash<QString, QString> frozen_;  // screen name → file
    QString last_;
};

} // namespace atrium
