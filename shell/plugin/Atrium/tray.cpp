#include "tray.hpp"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QMenu>
#include <QMutex>
#include <QPixmap>
#include <QUrl>
#include <QWindow>
#include <QtEndian>

namespace atrium {

namespace {

const QString kItem = QStringLiteral("org.kde.StatusNotifierItem");
const QString kWatcher = QStringLiteral("org.kde.StatusNotifierWatcher");
const QString kProps = QStringLiteral("org.freedesktop.DBus.Properties");
const QString kMenu = QStringLiteral("com.canonical.dbusmenu");

QMutex g_lock;
QHash<QString, QImage>& icons() {
    static QHash<QString, QImage> map;
    return map;
}

QVariant plain(const QVariant& v) {
    return v.canConvert<QDBusVariant>() ? v.value<QDBusVariant>().variant() : v;
}

// IconPixmap: a(iiay), ARGB32 in network byte order; the largest one.
QImage decode_pixmaps(const QVariant& v) {
    if (v.metaType() != QMetaType::fromType<QDBusArgument>())
        return {};
    const QDBusArgument arg = v.value<QDBusArgument>();
    QImage best;
    arg.beginArray();
    while (!arg.atEnd()) {
        int w = 0, h = 0;
        QByteArray data;
        arg.beginStructure();
        arg >> w >> h >> data;
        arg.endStructure();
        if (w <= 0 || h <= 0 || data.size() < qsizetype(w) * h * 4 || w <= best.width())
            continue;
        QImage img(w, h, QImage::Format_ARGB32);
        const auto* src = reinterpret_cast<const quint32*>(data.constData());
        for (int y = 0; y < h; ++y) {
            auto* line = reinterpret_cast<quint32*>(img.scanLine(y));
            for (int x = 0; x < w; ++x)
                line[x] = qFromBigEndian(src[y * w + x]);
        }
        best = img;
    }
    arg.endArray();
    return best;
}

// A theme icon from the item's own icon directory, if it brought one.
QString from_theme_path(const QString& dir, const QString& name) {
    if (dir.isEmpty() || name.isEmpty())
        return {};
    for (const char* ext : {".png", ".svg", ".xpm"}) {
        const QString direct = dir + "/" + name + ext;
        if (QFileInfo::exists(direct))
            return QUrl::fromLocalFile(direct).toString();
    }
    // A theme layout under it (hicolor/22x22/apps/name.png): the largest.
    QString found;
    int size = 0;
    QDir root(dir);
    const QStringList themes = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& theme : themes) {
        for (const QString& sz : QDir(root.filePath(theme)).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            for (const char* ext : {".png", ".svg"}) {
                const QString f = root.filePath(theme + "/" + sz + "/apps/" + name + ext);
                const int n = sz.section('x', 0, 0).toInt();
                if (QFileInfo::exists(f) && (n > size || sz == "scalable")) {
                    found = f;
                    size = sz == "scalable" ? 100000 : n;
                }
            }
        }
    }
    return found.isEmpty() ? QString() : QUrl::fromLocalFile(found).toString();
}

struct MenuNode {
    int id = 0;
    QVariantMap props;
    QList<MenuNode> children;
};

MenuNode read_node(const QDBusArgument& arg) {
    MenuNode n;
    arg.beginStructure();
    arg >> n.id >> n.props;
    arg.beginArray();
    while (!arg.atEnd()) {
        QDBusVariant v;
        arg >> v;
        n.children.append(read_node(v.variant().value<QDBusArgument>()));
    }
    arg.endArray();
    arg.endStructure();
    return n;
}

// dbusmenu mnemonics are "_", Qt's are "&".
QString qt_label(QString label) {
    label.replace("&", "&&");
    label.replace("__", QString(QChar(1)));
    label.replace("_", "&");
    label.replace(QChar(1), "_");
    return label;
}

void fill(QMenu* menu, const MenuNode& node, const QString& service, const QString& path) {
    for (const MenuNode& c : node.children) {
        if (c.props.contains("visible") && !plain(c.props.value("visible")).toBool())
            continue;
        if (plain(c.props.value("type")).toString() == "separator") {
            menu->addSeparator();
            continue;
        }
        const QString label = qt_label(plain(c.props.value("label")).toString());
        QAction* a;
        if (!c.children.isEmpty() || plain(c.props.value("children-display")).toString() == "submenu") {
            QMenu* sub = menu->addMenu(label);
            fill(sub, c, service, path);
            a = sub->menuAction();
        } else {
            a = menu->addAction(label);
            const int id = c.id;
            QObject::connect(a, &QAction::triggered, menu, [service, path, id] {
                QDBusMessage m = QDBusMessage::createMethodCall(service, path, kMenu, "Event");
                m << id << QStringLiteral("clicked") << QVariant::fromValue(QDBusVariant(0))
                  << uint(QDateTime::currentSecsSinceEpoch());
                QDBusConnection::sessionBus().asyncCall(m);
            });
        }
        if (c.props.contains("enabled"))
            a->setEnabled(plain(c.props.value("enabled")).toBool());
        const QString toggle = plain(c.props.value("toggle-type")).toString();
        if (toggle == "checkmark" || toggle == "radio") {
            a->setCheckable(true);
            a->setChecked(plain(c.props.value("toggle-state")).toInt() == 1);
        }
        if (const QString icon = plain(c.props.value("icon-name")).toString(); !icon.isEmpty()) {
            a->setIcon(QIcon::fromTheme(icon));
        } else if (const QByteArray png = plain(c.props.value("icon-data")).toByteArray(); !png.isEmpty()) {
            QPixmap p;
            if (p.loadFromData(png))
                a->setIcon(QIcon(p));
        }
    }
}

// "service/path" → (service, path).
std::pair<QString, QString> split_item(const QString& item) {
    const qsizetype slash = item.indexOf('/');
    if (slash < 0)
        return {item, QStringLiteral("/StatusNotifierItem")};
    return {item.left(slash), item.mid(slash)};
}

} // namespace

// --- icons -------------------------------------------------------------------

QImage TrayIcons::requestImage(const QString& id, QSize* size, const QSize& requested) {
    QMutexLocker lock(&g_lock);
    QImage img = icons().value(id.section('?', 0, 0));
    lock.unlock();
    if (!img.isNull() && requested.isValid() && !requested.isEmpty())
        img = img.scaled(requested, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (size)
        *size = img.size();
    return img;
}

void TrayIcons::put(const QString& key, const QImage& image) {
    QMutexLocker lock(&g_lock);
    icons().insert(key, image);
}

void TrayIcons::drop(const QString& key) {
    QMutexLocker lock(&g_lock);
    icons().remove(key);
}

// --- SystemTrayItem ----------------------------------------------------------

SystemTrayItem::SystemTrayItem(const QString& service, const QString& path, QObject* parent)
    : QObject(parent), service_(service), path_(path) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    for (const char* signal : {"NewIcon", "NewAttentionIcon", "NewOverlayIcon", "NewTitle", "NewToolTip", "NewStatus", "NewMenu"})
        bus.connect(service, path, kItem, signal, this, SLOT(refresh()));
    refresh();
}

SystemTrayItem::~SystemTrayItem() {
    TrayIcons::drop(key());
    if (shown_)
        shown_->deleteLater();
}

void SystemTrayItem::refresh() {
    QDBusMessage m = QDBusMessage::createMethodCall(service_, path_, kProps, "GetAll");
    m << kItem;
    auto* w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(m), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
        w->deleteLater();
        QDBusPendingReply<QVariantMap> r = *w;
        if (!r.isError())
            apply(r.value());
    });
}

void SystemTrayItem::apply(const QVariantMap& props) {
    id_ = plain(props.value("Id")).toString();
    title_ = plain(props.value("Title")).toString();
    status_ = plain(props.value("Status")).toString();
    menu_ = plain(props.value("Menu")).value<QDBusObjectPath>().path();
    onlyMenu_ = plain(props.value("ItemIsMenu")).toBool();
    themePath_ = plain(props.value("IconThemePath")).toString();

    const bool attention = status_ == "NeedsAttention";
    QString name = plain(props.value(attention ? "AttentionIconName" : "IconName")).toString();
    if (name.isEmpty())
        name = plain(props.value("IconName")).toString();
    QString source;
    if (name.startsWith('/'))
        source = QUrl::fromLocalFile(name).toString();
    if (source.isEmpty())
        source = from_theme_path(themePath_, name);
    if (source.isEmpty() && !name.isEmpty() && QIcon::hasThemeIcon(name))
        source = "image://icon/" + name;
    if (source.isEmpty()) {
        QImage img = decode_pixmaps(props.value(attention ? "AttentionIconPixmap" : "IconPixmap"));
        if (img.isNull())
            img = decode_pixmaps(props.value("IconPixmap"));
        if (!img.isNull()) {
            TrayIcons::put(key(), img);
            // A new URL each time, so the image reloads.
            source = QStringLiteral("image://trayicon/%1?%2").arg(key()).arg(++serial_);
        }
    }
    if (source.isEmpty())
        source = "image://icon/" + (name.isEmpty() ? QStringLiteral("application-x-executable") : name);
    icon_ = source;
    emit changed();
}

void SystemTrayItem::call(const QString& method, const QVariantList& args) {
    QDBusMessage m = QDBusMessage::createMethodCall(service_, path_, kItem, method);
    m.setArguments(args);
    QDBusConnection::sessionBus().asyncCall(m);
}

void SystemTrayItem::activate() {
    call("Activate", {0, 0});
}

void SystemTrayItem::secondaryActivate() {
    call("SecondaryActivate", {0, 0});
}

void SystemTrayItem::scroll(int delta, bool horizontal) {
    call("Scroll", {delta, horizontal ? QStringLiteral("horizontal") : QStringLiteral("vertical")});
}

void SystemTrayItem::display(QObject* window, int x, int y) {
    if (!hasMenu())
        return;
    if (shown_)
        shown_->close();
    QDBusConnection bus = QDBusConnection::sessionBus();
    // Apps that build their menu lazily do it now.
    QDBusMessage about = QDBusMessage::createMethodCall(service_, menu_, kMenu, "AboutToShow");
    about << 0;
    bus.call(about, QDBus::Block, 500);
    QDBusMessage get = QDBusMessage::createMethodCall(service_, menu_, kMenu, "GetLayout");
    get << 0 << -1 << QStringList();
    const QDBusMessage reply = bus.call(get, QDBus::Block, 1000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2)
        return;
    const MenuNode root = read_node(reply.arguments().at(1).value<QDBusArgument>());

    auto* menu = new QMenu;
    menu->setAttribute(Qt::WA_DeleteOnClose);
    fill(menu, root, service_, menu_);
    if (menu->isEmpty()) {
        delete menu;
        return;
    }
    auto* parent = qobject_cast<QWindow*>(window);
    menu->winId();
    if (parent)
        menu->windowHandle()->setTransientParent(parent);
    shown_ = menu;
    menu->popup(parent ? parent->mapToGlobal(QPoint(x, y)) : QPoint(x, y));
}

// --- TrayWatcher -------------------------------------------------------------

TrayWatcher::TrayWatcher(QObject* parent) : QObject(parent) {
    QDBusConnection::sessionBus().connect("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                          "NameOwnerChanged", this,
                                          SLOT(nameOwnerChanged(QString, QString, QString)));
}

void TrayWatcher::RegisterStatusNotifierItem(const QString& serviceOrPath) {
    // Ayatana items pass their object path; the rest their bus name.
    const QString item = serviceOrPath.startsWith('/') ? message().service() + serviceOrPath
                                                       : serviceOrPath + "/StatusNotifierItem";
    if (items_.contains(item))
        return;
    items_.append(item);
    emit StatusNotifierItemRegistered(item);
}

void TrayWatcher::RegisterStatusNotifierHost(const QString&) {
    emit StatusNotifierHostRegistered();
}

void TrayWatcher::nameOwnerChanged(const QString& name, const QString&, const QString& after) {
    if (!after.isEmpty())
        return;
    for (qsizetype i = items_.size() - 1; i >= 0; --i) {
        if (split_item(items_[i]).first == name) {
            const QString gone = items_.takeAt(i);
            emit StatusNotifierItemUnregistered(gone);
        }
    }
}

// --- SystemTray --------------------------------------------------------------

SystemTray* SystemTray::instance() {
    static auto* self = new SystemTray;
    return self;
}

SystemTray::SystemTray() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    // The watcher, unless another desktop part already is.
    if (bus.interface()->registerService(kWatcher, QDBusConnectionInterface::DontQueueService).value() ==
        QDBusConnectionInterface::ServiceRegistered) {
        watcher_ = new TrayWatcher(this);
        bus.registerObject("/StatusNotifierWatcher", watcher_,
                           QDBusConnection::ExportScriptableContents | QDBusConnection::ExportAllProperties);
        connect(watcher_, &TrayWatcher::StatusNotifierItemRegistered, this, &SystemTray::registered);
        connect(watcher_, &TrayWatcher::StatusNotifierItemUnregistered, this, &SystemTray::unregistered);
        return;
    }
    bus.connect(kWatcher, "/StatusNotifierWatcher", kWatcher, "StatusNotifierItemRegistered", this,
                SLOT(registered(QString)));
    bus.connect(kWatcher, "/StatusNotifierWatcher", kWatcher, "StatusNotifierItemUnregistered", this,
                SLOT(unregistered(QString)));
    const QString host = QStringLiteral("org.kde.StatusNotifierHost-%1").arg(QCoreApplication::applicationPid());
    bus.interface()->registerService(host);
    QDBusMessage reg = QDBusMessage::createMethodCall(kWatcher, "/StatusNotifierWatcher", kWatcher,
                                                      "RegisterStatusNotifierHost");
    reg << host;
    bus.asyncCall(reg);
    QDBusMessage get = QDBusMessage::createMethodCall(kWatcher, "/StatusNotifierWatcher", kProps, "Get");
    get << kWatcher << QStringLiteral("RegisteredStatusNotifierItems");
    auto* w = new QDBusPendingCallWatcher(bus.asyncCall(get), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
        w->deleteLater();
        QDBusPendingReply<QDBusVariant> r = *w;
        if (r.isError())
            return;
        for (const QString& item : r.value().variant().toStringList())
            add(item);
    });
}

void SystemTray::registered(const QString& item) {
    add(item);
}

void SystemTray::unregistered(const QString& item) {
    for (qsizetype i = 0; i < items_.size(); ++i) {
        const auto [service, path] = split_item(item);
        if (items_[i]->key() == service + path) {
            items_.takeAt(i)->deleteLater();
            shown_ = items();
            emit itemsChanged();
            return;
        }
    }
}

void SystemTray::add(const QString& item) {
    const auto [service, path] = split_item(item);
    for (SystemTrayItem* i : items_)
        if (i->key() == service + path)
            return;
    auto* i = new SystemTrayItem(service, path, this);
    // Its status decides whether it shows; the list changes only then.
    connect(i, &SystemTrayItem::changed, this, [this] {
        const QList<QObject*> now = items();
        if (now != shown_) {
            shown_ = now;
            emit itemsChanged();
        }
    });
    items_.append(i);
}

QList<QObject*> SystemTray::items() const {
    QList<QObject*> out;
    for (SystemTrayItem* i : items_)
        if (i->status() != "Passive" && !i->status().isEmpty())
            out.append(i);
    return out;
}

} // namespace atrium
