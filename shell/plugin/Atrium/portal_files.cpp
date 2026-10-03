#include "portal_files.hpp"

#include <QDBusArgument>
#include <QDBusMetaType>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace atrium {

QDBusArgument& operator<<(QDBusArgument& arg, const PortalFilterRule& r) {
    arg.beginStructure();
    arg << r.type << r.pattern;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, PortalFilterRule& r) {
    arg.beginStructure();
    arg >> r.type >> r.pattern;
    arg.endStructure();
    return arg;
}

QDBusArgument& operator<<(QDBusArgument& arg, const PortalFilter& f) {
    arg.beginStructure();
    arg << f.name << f.rules;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, PortalFilter& f) {
    arg.beginStructure();
    arg >> f.name >> f.rules;
    arg.endStructure();
    return arg;
}

namespace {

// An option the portal sends as a structure (or that Qt already read), as `T`.
template <typename T>
T read(const QVariant& v) {
    T out{};
    if (v.metaType() == QMetaType::fromType<QDBusArgument>())
        v.value<QDBusArgument>() >> out;
    else if (v.canConvert<T>())
        out = v.value<T>();
    return out;
}

// A path the portal sends as bytes (ay), NUL-terminated.
QString path(const QVariant& v) {
    QByteArray bytes = v.toByteArray();
    if (bytes.endsWith('\0'))
        bytes.chop(1);
    return QString::fromLocal8Bit(bytes);
}

QJsonObject filterJson(const PortalFilter& f) {
    QJsonArray patterns;
    for (const PortalFilterRule& r : f.rules)
        patterns.append(QJsonObject{{r.type == 1 ? "mime" : "glob", r.pattern}});
    return {{"name", f.name}, {"patterns", patterns}};
}

} // namespace

FileChooserAdaptor::FileChooserAdaptor(PortalBackend* parent) : QDBusAbstractAdaptor(parent) {
    qDBusRegisterMetaType<PortalFilterRule>();
    qDBusRegisterMetaType<QList<PortalFilterRule>>();
    qDBusRegisterMetaType<PortalFilter>();
    qDBusRegisterMetaType<PortalFilters>();
}

uint FileChooserAdaptor::OpenFile(const QDBusObjectPath& handle, const QString& app, const QString&,
                                  const QString& title, const QVariantMap& options, QVariantMap&) {
    ask("open", handle, app, title, options);
    return 2;  // unused: the reply goes later
}

uint FileChooserAdaptor::SaveFile(const QDBusObjectPath& handle, const QString& app, const QString&,
                                  const QString& title, const QVariantMap& options, QVariantMap&) {
    ask("save", handle, app, title, options);
    return 2;
}

uint FileChooserAdaptor::SaveFiles(const QDBusObjectPath& handle, const QString& app, const QString&,
                                   const QString& title, const QVariantMap& options, QVariantMap&) {
    ask("saveFiles", handle, app, title, options);
    return 2;
}

void FileChooserAdaptor::ask(const QString& mode, const QDBusObjectPath& handle, const QString& app,
                             const QString& title, const QVariantMap& options) {
    // The filters offered, and which is on: the app's current one is among
    // them, or the only one when it gave no list.
    PortalFilters filters = read<PortalFilters>(options.value("filters"));
    int current = -1;
    if (options.contains("current_filter")) {
        const auto f = read<PortalFilter>(options.value("current_filter"));
        current = int(filters.indexOf(f));
        if (current < 0) {
            filters.append(f);
            current = int(filters.size()) - 1;
        }
    }
    QJsonArray filterList;
    for (const PortalFilter& f : filters)
        filterList.append(filterJson(f));

    QJsonArray choices;
    for (const PortalChoice& c : read<PortalChoices>(options.value("choices"))) {
        QJsonArray opts;
        for (const PortalPair& o : c.options)
            opts.append(QJsonObject{{"id", o.id}, {"label", o.label}});
        choices.append(QJsonObject{{"id", c.id}, {"label", c.label}, {"options", opts}, {"value", c.initial}});
    }

    // Saving over a file starts in its folder, with its name.
    QString folder = path(options.value("current_folder"));
    QString name = options.value("current_name").toString();
    if (const QString file = path(options.value("current_file")); !file.isEmpty()) {
        folder = file.section('/', 0, -2);
        name = file.section('/', -1);
    }
    QJsonArray files;
    for (const QByteArray& f : read<QList<QByteArray>>(options.value("files")))
        files.append(path(f).section('/', -1));

    const QJsonObject question{
        {"mode", mode},
        {"app", app},
        {"title", title},
        {"accept", options.value("accept_label").toString()},
        {"multiple", options.value("multiple").toBool()},
        {"directory", options.value("directory").toBool()},
        {"filters", filterList},
        {"filter", current},
        {"choices", choices},
        {"folder", folder},
        {"name", name},
        {"files", files},
    };
    const bool open = mode == "open";
    // 0 chosen, 1 cancelled, 2 failed.
    askShell(handle.path(), "files.qml", QJsonDocument(question).toJson(QJsonDocument::Compact), {},
             [filters, open](const std::optional<QByteArray>& out) -> QVariantList {
                 if (!out)
                     return {uint(2), QVariantMap()};
                 const QJsonObject reply = QJsonDocument::fromJson(*out).object();
                 if (!reply.contains("uris"))
                     return {uint(1), QVariantMap()};
                 QVariantMap results;
                 results.insert("uris", reply.value("uris").toVariant().toStringList());
                 PortalPairs picked;
                 for (const QJsonValue& c : reply.value("choices").toArray())
                     picked.append({c.toArray().at(0).toString(), c.toArray().at(1).toString()});
                 results.insert("choices", QVariant::fromValue(picked));
                 if (const int f = reply.value("filter").toInt(-1); f >= 0 && f < filters.size())
                     results.insert("current_filter", QVariant::fromValue(filters[f]));
                 if (open)
                     results.insert("writable", true);
                 return {uint(0), results};
             });
}

} // namespace atrium
