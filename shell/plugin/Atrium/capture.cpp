#include "capture.hpp"

#include "capture_core.hpp"
#include "compositor.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>

#include <cstdio>

namespace atrium {

namespace {

QScreen* screenNamed(const QString& name) {
    for (QScreen* s : QGuiApplication::screens())
        if (s->name() == name)
            return s;
    return nullptr;
}

} // namespace

Capture::Capture(QObject* parent) : QObject(parent) {
    mode_ = qEnvironmentVariable("ATRIUM_CAPTURE_MODE", "toolbar");
    kind_ = mode_ == "window" || mode_ == "screen" || mode_ == "color" ? mode_ : QStringLiteral("region");
    freeze();
}

// Later, not now: the overlay asking for the shot goes away with it, and
// must finish its own handler first.
void Capture::pickingMayChange() {
    QMetaObject::invokeMethod(this, &Capture::pickingChanged, Qt::QueuedConnection);
}

void Capture::setKind(const QString& kind) {
    if (kind == kind_)
        return;
    kind_ = kind;
    emit kindChanged();
}

void Capture::freeze() {
    // Not interactive: the whole desktop as one picture, at once.
    if (mode_ == "portal") {
        auto* grim = new QProcess(this);
        const QString file = dir_.filePath("all.png");
        connect(grim, &QProcess::finished, this, [this, file](int code) {
            if (code != 0)
                return answer({});
            deliver(QImage(file));
        });
        grim->start("grim", {"-l", "1", file});
        return;
    }
    const QList<QScreen*> screens = QGuiApplication::screens();
    auto pending = std::make_shared<int>(int(screens.size()));
    for (QScreen* s : screens) {
        const QString name = s->name();
        const QString file = dir_.filePath(name + ".png");
        auto* grim = new QProcess(this);
        connect(grim, &QProcess::finished, this, [this, grim, name, file, pending](int code) {
            grim->deleteLater();
            if (code == 0)
                frozen_.insert(name, file);
            if (--*pending == 0) {
                ready_ = true;
                emit readyChanged();
                pickingMayChange();
                if (mode_ == "screen")
                    takeScreen({});
            }
        });
        grim->start("grim", {"-l", "0", "-o", name, file});
    }
}

QVariantMap Capture::frozen() const {
    QVariantMap out;
    for (auto it = frozen_.begin(); it != frozen_.end(); ++it)
        out.insert(it.key(), QUrl::fromLocalFile(it.value()));
    return out;
}

QVariantMap Capture::windowAt(const QString& screen, int x, int y) const {
    QScreen* s = screenNamed(screen);
    if (!s)
        return {};
    Compositor* c = Compositor::instance();
    const QVariant secret = c->shownSecret();
    const QString secretName = secret.toMap().value("name").toString();
    // What's showing, top first: a shown secret space over the rest, then
    // windows kept above, then by when they were last used.
    QList<QVariantMap> shown;
    for (int pass = 0; pass < 3; ++pass) {
        for (const QVariant& v : c->windows()) {
            const QVariantMap w = v.toMap();
            if (w.value("minimized").toBool() || w.value("identifier").toString().isEmpty())
                continue;
            const bool inSecret = w.value("secret").toBool();
            const QString output = w.value("output").toString();
            const bool onScreen = !inSecret && (w.value("sticky").toBool() ||
                                                w.value("space").toString() == QString::number(c->activeSpace(output)));
            const bool mine = pass == 0 ? inSecret && w.value("space").toString() == secretName
                            : pass == 1 ? onScreen && w.value("keep_above").toBool()
                                        : onScreen && !w.value("keep_above").toBool();
            if (mine)
                shown.append(w);
        }
    }
    std::vector<capture::Box> boxes;
    for (const QVariantMap& w : shown) {
        const QVariantMap g = w.value("geometry").toMap();
        boxes.push_back({g.value("x").toInt(), g.value("y").toInt(), g.value("width").toInt(),
                         g.value("height").toInt()});
    }
    const QRect sg = s->geometry();
    const int i = capture::topmost_at(boxes, sg.x() + x, sg.y() + y);
    if (i < 0)
        return {};
    const QVariantMap& w = shown[i];
    const capture::Box& b = boxes[size_t(i)];
    const int bar = w.value("title_bar").toInt();
    return {
        {"identifier", w.value("identifier")},
        {"title", w.value("title")},
        {"appId", w.value("app_id")},
        {"x", b.x - sg.x()},
        {"y", b.y + bar - sg.y()},
        {"width", b.width},
        {"height", b.height - bar},
    };
}

QString Capture::colorAt(const QString& screen, int x, int y) const {
    QScreen* s = screenNamed(screen);
    const QImage image(frozen_.value(screen));
    if (!s || image.isNull())
        return {};
    const capture::Box p = capture::to_pixels({x, y, 1, 1}, s->geometry().width(), s->geometry().height(),
                                              image.width(), image.height());
    return p.width > 0 ? image.pixelColor(p.x, p.y).name() : QString();
}

void Capture::takeRegion(const QString& screen, int x0, int y0, int x1, int y1) {
    QScreen* s = screenNamed(screen);
    const QImage image(frozen_.value(screen));
    if (!s || image.isNull())
        return;
    const capture::Box b = capture::between(x0, y0, x1, y1);
    const capture::Box p = capture::to_pixels(b, s->geometry().width(), s->geometry().height(), image.width(),
                                              image.height());
    if (p.width < 2 || p.height < 2)
        return;
    deliver(image.copy(p.x, p.y, p.width, p.height));
}

void Capture::takeScreen(const QString& screen) {
    if (!screen.isEmpty()) {
        deliver(QImage(frozen_.value(screen)));
        return;
    }
    // All of them where they sit, at the largest scale among them.
    const QList<QScreen*> screens = QGuiApplication::screens();
    QRect all;
    qreal scale = 1;
    for (QScreen* s : screens) {
        all |= s->geometry();
        const QImage image(frozen_.value(s->name()));
        if (!image.isNull() && s->geometry().width() > 0)
            scale = std::max(scale, qreal(image.width()) / s->geometry().width());
    }
    if (screens.size() == 1) {
        deliver(QImage(frozen_.value(screens.first()->name())));
        return;
    }
    QImage out(all.size() * scale, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (QScreen* s : screens) {
        const QRect g = s->geometry().translated(-all.topLeft());
        painter.drawImage(QRectF(g.x() * scale, g.y() * scale, g.width() * scale, g.height() * scale),
                          QImage(frozen_.value(s->name())));
    }
    painter.end();
    deliver(out);
}

void Capture::takeWindow(const QString& identifier, const QString& screen, int x, int y, int width, int height) {
    const QString file = dir_.filePath("window.png");
    auto* grim = new QProcess(this);
    busy_ = true;
    pickingMayChange();
    connect(grim, &QProcess::finished, this, [=, this](int code) {
        grim->deleteLater();
        busy_ = false;
        const QImage image = code == 0 ? QImage(file) : QImage();
        if (!image.isNull()) {
            deliver(image);
        } else {
            qWarning("capture: grim couldn't copy window %s: %s", qPrintable(identifier),
                     grim->readAllStandardError().trimmed().constData());
            if (!screen.isEmpty() && width > 1 && height > 1)
                takeRegion(screen, x, y, x + width, y + height);
            else
                emit failed("The window couldn't be captured.");
        }
        pickingMayChange();
    });
    grim->start("grim", {"-l", "0", "-T", identifier, file});
}

void Capture::deliver(const QImage& image) {
    if (image.isNull() || done_)
        return;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/Screenshots";
    QDir().mkpath(dir);
    const QDateTime now = QDateTime::currentDateTime();
    const std::tm when{now.time().second(), now.time().minute(), now.time().hour(), now.date().day(),
                       now.date().month() - 1, now.date().year() - 1900};
    QString file = dir + "/" + QString::fromStdString(capture::file_name(when));
    for (int n = 2; QFile::exists(file); ++n)  // two in the same second
        file = dir + "/" + QString::fromStdString(capture::file_name(when)).chopped(4) + QString(" (%1).png").arg(n);
    if (!image.save(file, "PNG")) {
        emit failed("The screenshot couldn't be saved.");
        return;
    }
    last_ = file;
    pickingMayChange();
    if (forPortal())
        return answer(QUrl::fromLocalFile(file).toString(QUrl::FullyEncoded));
    // And on the clipboard, as a picture.
    auto* copy = new QProcess(this);
    connect(copy, &QProcess::finished, copy, &QObject::deleteLater);
    copy->setStandardInputFile(file);
    copy->start("wl-copy", {"--type", "image/png"});
    emit taken();
}

void Capture::pickColor(const QString& screen, int x, int y) {
    const QColor c(colorAt(screen, x, y));
    if (!c.isValid())
        return answer({});
    answer(QString("%1 %2 %3").arg(c.redF()).arg(c.greenF()).arg(c.blueF()));
}

void Capture::answer(const QString& line) {
    if (done_)
        return;
    done_ = true;
    if (!line.isEmpty()) {
        std::fputs((line + u'\n').toUtf8().constData(), stdout);
        std::fflush(stdout);
    }
    QCoreApplication::quit();
}

void Capture::cancel() {
    if (forPortal())
        return answer({});
    done_ = true;
    QCoreApplication::quit();
}

void Capture::record() {
    Compositor::instance()->action("shell", "record");
    done_ = true;
    QCoreApplication::quit();
}

void Capture::deleteLast() {
    if (!last_.isEmpty())
        QFile::remove(last_);
    done_ = true;
    QCoreApplication::quit();
}

} // namespace atrium
