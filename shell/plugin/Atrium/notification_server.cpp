#include "notification_server.hpp"

#include "compositor.hpp"
#include "desktop_entries.hpp"
#include "notifications.hpp"
#include "version.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDateTime>
#include <QFileInfo>
#include <QIcon>
#include <QMutex>
#include <QUrl>

namespace atrium {

namespace {

constexpr int kMaxPopups = 5;

QMutex g_images_lock;
QHash<QString, QImage>& images() {
    static QHash<QString, QImage> map;
    return map;
}

bool is_path(const QString& s) {
    return s.startsWith('/') || s.startsWith("file://");
}

QString file_url(const QString& s) {
    return s.startsWith('/') ? QUrl::fromLocalFile(s).toString() : s;
}

QString themed(const QString& name) {
    return !name.isEmpty() && QIcon::hasThemeIcon(name) ? "image://icon/" + name : QString();
}

// image-data: (iiibiiay) width, height, rowstride, has alpha, bits per
// sample, channels, pixels.
QImage decode_pixels(const QVariant& v) {
    if (!v.canConvert<QDBusArgument>())
        return {};
    const QDBusArgument arg = v.value<QDBusArgument>();
    int w = 0, h = 0, stride = 0, bits = 0, channels = 0;
    bool alpha = false;
    QByteArray data;
    arg.beginStructure();
    arg >> w >> h >> stride >> alpha >> bits >> channels >> data;
    arg.endStructure();
    if (w <= 0 || h <= 0 || bits != 8 || (channels != 3 && channels != 4) || data.size() < qsizetype(stride) * (h - 1) + w * channels)
        return {};
    const QImage::Format fmt = channels == 4 ? QImage::Format_RGBA8888 : QImage::Format_RGB888;
    return QImage(reinterpret_cast<const uchar*>(data.constData()), w, h, stride, fmt).copy();
}

QVariant hint(const QVariantMap& hints, std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (auto it = hints.find(n); it != hints.end())
            return *it;
    return {};
}

} // namespace

// --- images ------------------------------------------------------------------

QImage NotificationImages::requestImage(const QString& id, QSize* size, const QSize& requested) {
    QMutexLocker lock(&g_images_lock);
    QImage img = images().value(id);
    lock.unlock();
    if (!img.isNull() && requested.isValid() && !requested.isEmpty())
        img = img.scaled(requested, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (size)
        *size = img.size();
    return img;
}

void NotificationImages::put(const QString& key, const QImage& image) {
    QMutexLocker lock(&g_images_lock);
    images().insert(key, image);
}

void NotificationImages::drop(const QString& key) {
    QMutexLocker lock(&g_images_lock);
    images().remove(key);
}

// --- server ------------------------------------------------------------------

NotificationServer* NotificationServer::instance() {
    static auto* self = new NotificationServer;
    return self;
}

NotificationServer::NotificationServer() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerObject("/org/freedesktop/Notifications", this,
                            QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals)) {
        qWarning("notifications: couldn't export the server object");
        return;
    }
    // Taken over from whatever held it (a daemon D-Bus activated before
    // the shell was up), and handed on if another asks.
    const auto reply = bus.interface()->registerService("org.freedesktop.Notifications",
        QDBusConnectionInterface::ReplaceExistingService, QDBusConnectionInterface::AllowReplacement);
    if (!reply.isValid() || reply.value() != QDBusConnectionInterface::ServiceRegistered)
        qWarning("notifications: another server keeps org.freedesktop.Notifications");
}

QList<QObject*> NotificationServer::popups() const {
    QList<QObject*> out;
    for (const QPointer<Notification>& n : popups_)
        if (n)
            out.append(n);
    return out;
}

Notification::Data NotificationServer::resolve(const QString& app_name, const QString& app_icon,
                                               const QStringList& actions, const QVariantMap& hints,
                                               int expire_timeout, uint id) {
    Notification::Data d;
    auto* entries = shell::DesktopEntries::instance();
    d.desktopEntry = hint(hints, {"desktop-entry"}).toString();
    shell::DesktopEntry* entry = entries->byId(d.desktopEntry);
    if (!entry)
        entry = entries->byId(app_name.toLower());

    d.app = !app_name.isEmpty() ? app_name : entry ? entry->name() : QStringLiteral("Notification");
    d.urgency = hint(hints, {"urgency"}).toInt();
    if (!hints.contains("urgency"))
        d.urgency = 1;
    d.resident = hint(hints, {"resident"}).toBool();
    d.timeout = expire_timeout > 0 ? expire_timeout : 5000;

    // The picture: pixels, or a file. image-path may instead name an icon.
    const QString key = QStringLiteral("%1-%2").arg(id).arg(QDateTime::currentMSecsSinceEpoch());
    QString image_icon;
    if (QImage img = decode_pixels(hint(hints, {"image-data", "image_data"})); !img.isNull()) {
        NotificationImages::put(key, img);
        d.image = "image://notification/" + key;
    } else if (const QString path = hint(hints, {"image-path", "image_path"}).toString(); !path.isEmpty()) {
        if (is_path(path))
            d.image = file_url(path);
        else
            image_icon = path;
    }

    // The icon: the app's icon if the theme has it (or the icon it named as
    // its image), else its desktop entry's; a generic one rather than a
    // wrong guess.
    if (is_path(app_icon))
        d.icon = file_url(app_icon);
    if (d.icon.isEmpty())
        d.icon = themed(app_icon);
    if (d.icon.isEmpty() && !image_icon.isEmpty()) {
        d.icon = themed(image_icon);
        // Discord's icon lives in pixmaps, reachable through its app.
        if (d.icon.isEmpty())
            if (auto* e = entries->heuristicLookup(image_icon))
                d.icon = "image://icon/" + e->icon();
    }
    if (d.icon.isEmpty())
        if (QImage img = decode_pixels(hint(hints, {"icon_data"})); !img.isNull()) {
            NotificationImages::put(key + "-icon", img);
            d.icon = "image://notification/" + key + "-icon";
        }
    if (d.icon.isEmpty() && entry && !entry->icon().isEmpty())
        d.icon = is_path(entry->icon()) ? file_url(entry->icon()) : "image://icon/" + entry->icon();
    if (d.icon.isEmpty())
        d.icon = themed("preferences-desktop-notification");
    if (d.icon.isEmpty())
        d.icon = "image://icon/dialog-information";

    for (qsizetype i = 0; i + 1 < actions.size(); i += 2) {
        if (actions[i] == "default") {
            d.hasDefault = true;
            continue;
        }
        d.actions.append(QVariantMap{{"id", actions[i]}, {"text", actions[i + 1]}});
    }
    return d;
}

uint NotificationServer::Notify(const QString& app_name, uint replaces_id, const QString& app_icon,
                                const QString& summary, const QString& body, const QStringList& actions,
                                const QVariantMap& hints, int expire_timeout) {
    const uint id = replaces_id && live_.contains(replaces_id) ? replaces_id : next_++;
    Notification::Data d = resolve(app_name, app_icon, actions, hints, expire_timeout, id);
    d.summary = summary;
    d.body = body;
    // Turned off in Settings: not kept, not shown.
    const QString mode = NotificationHistory::modeOf(d.app);
    if (mode == "off")
        return id;

    NotificationHistory::instance()->add({
        {"app", d.app}, {"icon", d.icon}, {"summary", summary}, {"body", body},
        {"image", d.image}, {"urgency", d.urgency}, {"desktopEntry", d.desktopEntry},
    });

    if (Notification* n = live_.value(id)) {
        n->update(std::move(d));  // replaced in place, where it is
        return id;
    }
    auto* n = new Notification(id, std::move(d), this);
    live_.insert(id, n);
    const bool dnd = Compositor::instance()->settings().value("notifications.dnd").toBool();
    if ((!dnd && mode != "quiet") || n->critical()) {
        popups_.prepend(n);
        // The oldest beyond the limit leave the screen.
        while (popups_.size() > kMaxPopups) {
            QPointer<Notification> old = popups_.takeLast();
            if (old)
                close(old->id(), 1);
        }
        emit popupsChanged();
    } else {
        // Not shown: it lives in the history only.
        close(id, 1);
    }
    return id;
}

void NotificationServer::close(uint id, uint reason) {
    Notification* n = live_.take(id);
    if (!n)
        return;
    const qsizetype before = popups_.size();
    popups_.removeIf([n](const QPointer<Notification>& p) { return !p || p == n; });
    NotificationImages::drop(n->image().section('/', -1));
    emit NotificationClosed(id, reason);
    emit n->closed();
    if (popups_.size() != before)
        emit popupsChanged();
    n->deleteLater();
}

void NotificationServer::CloseNotification(uint id) {
    close(id, 3);
}

QStringList NotificationServer::GetCapabilities() {
    return {"body", "body-markup", "actions", "icon-static", "persistence"};
}

QString NotificationServer::GetServerInformation(QString& vendor, QString& version, QString& spec_version) {
    vendor = "atrium";
    version = QStringLiteral(ATRIUM_VERSION);
    spec_version = "1.2";
    return "atrium";
}

void NotificationServer::expire(Notification* n) {
    if (n)
        close(n->id(), 1);
}

void NotificationServer::dismiss(Notification* n) {
    if (n)
        close(n->id(), 2);
}

namespace {

// A click brings the app that sent it forward, as on a Mac: the action it
// answers (an updater, a chat) is in its window, maybe in a hidden space.
void raise_sender(const Notification::Data& d) {
    const QString wanted = (d.desktopEntry.isEmpty() ? d.app : d.desktopEntry).toLower();
    if (wanted.isEmpty())
        return;
    for (const QVariant& w : Compositor::instance()->windows()) {
        const QVariantMap m = w.toMap();
        const QString id = m.value("app_id").toString().toLower();
        if (id == wanted || id.endsWith("." + wanted)) {
            Compositor::instance()->focusWindow(m.value("id").toInt());
            return;
        }
    }
}

} // namespace

void NotificationServer::activate(Notification* n) {
    if (!n)
        return;
    if (n->hasDefault()) {
        invoke(n, "default");
    } else {
        raise_sender(n->data());
        close(n->id(), 2);
    }
}

void NotificationServer::invoke(Notification* n, const QString& action) {
    if (!n)
        return;
    emit ActionInvoked(n->id(), action);
    raise_sender(n->data());
    // Done with, unless it stays by design.
    if (!n->data().resident)
        close(n->id(), 2);
}

} // namespace atrium
