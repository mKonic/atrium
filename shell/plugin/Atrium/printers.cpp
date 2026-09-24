#include "printers.hpp"

#include "printers_core.hpp"

#include <QLocale>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>

namespace atrium {

namespace {

// A CUPS tool's output in plain English, so it parses.
std::string output(const QString& program, const QStringList& args) {
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("LC_ALL", "C");
    p.setProcessEnvironment(env);
    p.start(program, args);
    if (!p.waitForFinished(3000))
        return {};
    return p.readAllStandardOutput().toStdString();
}

} // namespace

Printers::Printers(QObject* parent) : QObject(parent) {
    timer_.setInterval(3000);
    connect(&timer_, &QTimer::timeout, this, &Printers::refresh);
    refresh();
}

void Printers::setWatching(bool on) {
    if (on == watching())
        return;
    if (on) {
        refresh();
        timer_.start();
    } else {
        timer_.stop();
    }
    emit watchingChanged();
}

void Printers::refresh() {
    auto installed = printers::parse_printers(output("lpstat", {"-l", "-p"}));
    const std::string def = printers::parse_default(output("lpstat", {"-d"}));
    // Found on the network and usable without setting up (temporary queues).
    std::vector<printers::Printer> all = installed;
    QSet<QString> names;
    for (const auto& p : installed)
        names.insert(QString::fromStdString(p.name));
    for (const QString& line : QString::fromStdString(output("lpstat", {"-e"})).split('\n', Qt::SkipEmptyParts))
        if (!names.contains(line.trimmed()))
            all.push_back({line.trimmed().toStdString(), "", "idle", true});
    const auto jobs = printers::parse_jobs(output("lpstat", {"-o"}), all);

    QVariantList out;
    for (const auto& p : all) {
        QVariantList queue;
        for (const auto& j : jobs)
            if (j.printer == p.name)
                queue.append(QVariantMap{{"id", QString::fromStdString(j.id)},
                                         {"user", QString::fromStdString(j.user)},
                                         {"size", QLocale().formattedDataSize(j.size)}});
        const QString name = QString::fromStdString(p.name);
        out.append(QVariantMap{{"name", name},
                               {"label", p.description.empty() ? QString(name).replace('_', ' ')
                                                               : QString::fromStdString(p.description)},
                               {"state", QString::fromStdString(p.state)},
                               {"isDefault", p.name == def},
                               {"installed", names.contains(name)},
                               {"jobs", queue}});
    }
    if (out != printers_) {
        printers_ = out;
        emit changed();
    }
}

void Printers::run(const QString& program, const QStringList& args) {
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p](int code) {
        p->deleteLater();
        // 126/127: pkexec's "not allowed" and "cancelled", said by the dialog already.
        if (code != 0 && code != 126 && code != 127)
            emit failed(QString::fromUtf8(p->readAllStandardError()).trimmed());
        refresh();
    });
    p->start(program, args);
}

void Printers::setDefault(const QString& name) {
    run("lpoptions", {"-d", name});
}

void Printers::cancel(const QString& job) {
    run("cancel", {job});
}

void Printers::add(const QString& name, const QString& address) {
    QString uri = address.trimmed();
    if (!uri.contains("://"))
        uri = "ipp://" + uri + "/ipp/print";
    // CUPS names: letters, digits, dashes and underscores.
    QString queue = name.trimmed();
    queue.replace(QRegularExpression("[^A-Za-z0-9_-]+"), "_");
    if (queue.isEmpty())
        return;
    run("pkexec", {"lpadmin", "-p", queue, "-E", "-v", uri, "-m", "everywhere"});
}

void Printers::remove(const QString& name) {
    run("pkexec", {"lpadmin", "-x", name});
}

} // namespace atrium
