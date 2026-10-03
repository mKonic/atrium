#include "print_prompt.hpp"

#include "files.hpp"
#include "printers_core.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#include <unistd.h>

#include <cstdio>

namespace atrium {

namespace {

// CUPS's tools, in the C locale (their output is parsed).
void runTool(const QString& program, const QStringList& args, std::function<void(const QByteArray&)> done) {
    auto* p = new QProcess;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("LC_ALL", "C");
    p->setProcessEnvironment(env);
    QObject::connect(p, &QProcess::finished, p, [p, done](int code, QProcess::ExitStatus status) {
        done(status == QProcess::NormalExit && code == 0 ? p->readAllStandardOutput() : QByteArray());
        p->deleteLater();
    });
    QObject::connect(p, &QProcess::errorOccurred, p, [p, done](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            done({});
            p->deleteLater();
        }
    });
    p->start(program, args);
}

QVariantMap choice(const QString& value, const QString& label) {
    return {{"value", value}, {"label", label}};
}

QString duplexLabel(const QString& v) {
    if (v == "None")
        return "Off";
    if (v == "DuplexNoTumble")
        return "Flip on long edge";
    if (v == "DuplexTumble")
        return "Flip on short edge";
    return v;
}

QString colorLabel(const QString& v) {
    if (v == "Gray" || v == "Grayscale" || v == "Black" || v == "KGray")
        return "Black & white";
    if (v == "RGB" || v == "CMYK" || v == "Color" || v == "CMY")
        return "Colour";
    return v;
}

bool isGray(const QString& v) {
    return colorLabel(v) == "Black & white";
}

QString paperLabel(const QString& ppd) {
    const printers::Paper* p = printers::paper_by_ppd(ppd.toStdString());
    return p ? QString::fromUtf8(p->display.data(), qsizetype(p->display.size())) : ppd;
}

QString plainLabel(const QString& label) {
    QString out;
    for (qsizetype i = 0; i < label.size(); ++i) {
        if (label[i] == '_' && i + 1 < label.size())
            ++i;
        else if (label[i] == '_')
            continue;
        out += label[i];
    }
    return out;
}

bool hasValue(const QVariantList& list, const QString& value) {
    for (const QVariant& v : list)
        if (v.toMap().value("value") == value)
            return true;
    return false;
}

} // namespace

PrintPrompt::PrintPrompt(QObject* parent) : QObject(parent), run_(runTool) {
    QByteArray request;
    if (!isatty(STDIN_FILENO)) {
        QFile in;
        if (in.open(stdin, QIODevice::ReadOnly))
            request = in.readAll();
    }
    read(request);
    loadPrinters();
}

PrintPrompt::PrintPrompt(const QByteArray& request, Run run, QObject* parent)
    : QObject(parent), run_(std::move(run)), quit_(false) {
    read(request);
    loadPrinters();
}

void PrintPrompt::read(const QByteArray& request) {
    const QJsonObject q = QJsonDocument::fromJson(request).object();
    title_ = q.value("title").toString();
    accept_ = plainLabel(q.value("accept").toString());
    if (accept_.isEmpty())
        accept_ = "Print";
    asked_ = q.value("settings").toObject().toVariantMap();
    copies_ = qMax(1, asked_.value("n-copies").toString().toInt());
    landscape_ = asked_.value("orientation").toString().contains("landscape") ||
                 q.value("pageSetup").toObject().value("Orientation").toString().contains("landscape");
    if (const auto* p = printers::paper_by_pwg(asked_.value("paper-format").toString().toStdString()))
        paper_ = QString::fromUtf8(p->ppd.data(), qsizetype(p->ppd.size()));
    // A PDF to save: in Documents, named for what's printed, not over another.
    QString name = title_.trimmed();
    name.replace('/', '-');
    if (name.isEmpty())
        name = "Document";
    if (!name.endsWith(".pdf", Qt::CaseInsensitive))
        name += ".pdf";
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!QFileInfo(dir).isDir())
        dir = QDir::homePath();
    const QDir d(dir);
    pdfFile_ = d.filePath(QString::fromStdString(files::free_file_name(
        name.toStdString(), [&](const std::string& n) { return d.exists(QString::fromStdString(n)); })));
}

void PrintPrompt::loadPrinters() {
    run_("lpstat", {"-l", "-p"}, [this](const QByteArray& list) {
        run_("lpstat", {"-d"}, [this, list](const QByteArray& def) {
            const auto found = printers::parse_printers(list.toStdString());
            printers_.clear();
            for (const auto& p : found) {
                const QString name = QString::fromStdString(p.name);
                const QString desc = QString::fromStdString(p.description);
                printers_.append(choice(name, desc.isEmpty() ? name : desc));
            }
            printers_.append(choice("pdf", "Save as PDF"));
            emit printersChanged();
            // The one the app last used, else the default, else the first.
            const QString asked = asked_.value("printer").toString();
            const QString fallback = QString::fromStdString(printers::parse_default(def.toStdString()));
            if (asked_.contains("output-uri") && !asked_.value("output-uri").toString().isEmpty())
                setPrinter("pdf");
            else if (hasValue(printers_, asked))
                setPrinter(asked);
            else if (hasValue(printers_, fallback))
                setPrinter(fallback);
            else
                setPrinter(printers_.first().toMap().value("value").toString());
        });
    });
}

void PrintPrompt::setPrinter(const QString& name) {
    if (name == printer_ || !hasValue(printers_, name))
        return;
    printer_ = name;
    emit printerChanged();
    loadOptions();
}

void PrintPrompt::loadOptions() {
    auto apply = [this](const std::vector<printers::Option>& options) {
        papers_.clear();
        duplexes_.clear();
        colors_.clear();
        QString paperNow, duplexNow, colorNow;
        for (const auto& o : options) {
            QVariantList* list = o.key == "PageSize" ? &papers_ : o.key == "Duplex" ? &duplexes_
                                 : o.key == "ColorModel"                         ? &colors_
                                                                                 : nullptr;
            if (!list)
                continue;
            for (const std::string& c : o.choices) {
                // "Custom.WIDTHxHEIGHT" is where a size would be typed in, not one.
                if (c.starts_with("Custom."))
                    continue;
                const QString v = QString::fromStdString(c);
                list->append(choice(v, o.key == "PageSize" ? paperLabel(v)
                                       : o.key == "Duplex" ? duplexLabel(v)
                                                           : colorLabel(v)));
            }
            const QString current = QString::fromStdString(o.current);
            (o.key == "PageSize" ? paperNow : o.key == "Duplex" ? duplexNow : colorNow) = current;
        }
        // What the app asked for, where this printer has it; else its own.
        if (!hasValue(papers_, paper_))
            paper_ = paperNow;
        const QString askedDuplex = asked_.value("duplex").toString();
        duplex_ = askedDuplex == "horizontal" && hasValue(duplexes_, "DuplexNoTumble") ? "DuplexNoTumble"
                  : askedDuplex == "vertical" && hasValue(duplexes_, "DuplexTumble")  ? "DuplexTumble"
                  : askedDuplex == "simplex" && hasValue(duplexes_, "None")           ? "None"
                                                                                       : duplexNow;
        color_ = colorNow;
        if (asked_.value("use-color").toString() == "false")
            for (const QVariant& c : std::as_const(colors_))
                if (isGray(c.toMap().value("value").toString()))
                    color_ = c.toMap().value("value").toString();
        emit optionsChanged();
        emit changed();
    };
    if (pdf()) {
        // Any paper; the one for here unless the app said.
        std::vector<printers::Option> options(1);
        options[0].key = "PageSize";
        for (const auto& p : printers::papers())
            options[0].choices.emplace_back(p.ppd);
        options[0].current = QLocale().measurementSystem() == QLocale::ImperialUSSystem ? "Letter" : "A4";
        apply(options);
        return;
    }
    const QString name = printer_;
    run_("lpoptions", {"-p", name, "-l"}, [this, name, apply](const QByteArray& out) {
        if (name == printer_)  // not since changed again
            apply(printers::parse_options(out.toStdString()));
    });
}

void PrintPrompt::setPaper(const QString& v) {
    if (v != paper_) {
        paper_ = v;
        emit changed();
    }
}

void PrintPrompt::setDuplex(const QString& v) {
    if (v != duplex_) {
        duplex_ = v;
        emit changed();
    }
}

void PrintPrompt::setColor(const QString& v) {
    if (v != color_) {
        color_ = v;
        emit changed();
    }
}

void PrintPrompt::setCopies(int n) {
    if (n != copies_) {
        copies_ = n;
        emit changed();
    }
}

void PrintPrompt::setPages(const QString& v) {
    if (v != pages_) {
        pages_ = v;
        emit changed();
    }
}

void PrintPrompt::setLandscape(bool on) {
    if (on != landscape_) {
        landscape_ = on;
        emit changed();
    }
}

void PrintPrompt::setPdfFile(const QString& v) {
    if (v != pdfFile_) {
        pdfFile_ = v;
        emit changed();
    }
}

QString PrintPrompt::problem() const {
    if (printer_.isEmpty())
        return "Looking for printers…";
    if (copies_ < 1 || copies_ > 999)
        return "Print 1 to 999 copies.";
    if (!pages_.trimmed().isEmpty() && !printers::zero_based_ranges(pages_.toStdString()))
        return "Pages are numbers from 1, like 1-3, 5.";
    if (pdf()) {
        const QFileInfo f(pdfFile_.trimmed());
        if (pdfFile_.trimmed().isEmpty() || f.isDir())
            return "Name the PDF to save.";
        if (!QFileInfo(f.absolutePath()).isWritable())
            return "You can't save in that folder.";
    }
    return {};
}

void PrintPrompt::print() {
    if (answered_ || !problem().isEmpty())
        return;
    QJsonObject settings;
    for (auto it = asked_.begin(); it != asked_.end(); ++it)
        settings.insert(it.key(), it.value().toString());
    const auto ranges = printers::zero_based_ranges(pages_.toStdString());
    settings["n-copies"] = QString::number(copies_);
    settings["collate"] = "true";
    settings["print-pages"] = ranges ? "ranges" : "all";
    if (ranges)
        settings["page-ranges"] = QString::fromStdString(*ranges);
    else
        settings.remove("page-ranges");
    settings["orientation"] = landscape_ ? "landscape" : "portrait";
    const printers::Paper* paper = printers::paper_by_ppd(paper_.toStdString());
    if (paper)
        settings["paper-format"] = QString::fromUtf8(paper->pwg.data(), qsizetype(paper->pwg.size()));
    if (!duplexes_.isEmpty())
        settings["duplex"] = duplex_ == "DuplexNoTumble" ? "horizontal" : duplex_ == "DuplexTumble" ? "vertical" : "simplex";
    if (!colors_.isEmpty())
        settings["use-color"] = isGray(color_) ? "false" : "true";
    const QString file = QFileInfo(pdfFile_.trimmed()).absoluteFilePath();
    if (pdf()) {
        settings["printer"] = "Print to File";
        settings["output-uri"] = QUrl::fromLocalFile(file).toString();
        settings["output-file-format"] = "pdf";
    } else {
        settings["printer"] = printer_;
        settings.remove("output-uri");
        settings.remove("output-file-format");
    }

    QJsonObject setup{{"Orientation", landscape_ ? "landscape" : "portrait"}};
    if (paper) {
        setup["PPDName"] = QString::fromUtf8(paper->ppd.data(), qsizetype(paper->ppd.size()));
        setup["Name"] = QString::fromUtf8(paper->pwg.data(), qsizetype(paper->pwg.size()));
        setup["DisplayName"] = QString::fromUtf8(paper->display.data(), qsizetype(paper->display.size()));
        setup["Width"] = paper->width;
        setup["Height"] = paper->height;
    }

    const QJsonObject job{
        {"printer", pdf() ? QString() : printer_},
        {"pdf", pdf() ? file : QString()},
        {"copies", copies_},
        {"paper", paper_},
        {"duplex", duplex_},
        {"color", color_},
    };
    answer_ = QJsonDocument(QJsonObject{{"settings", settings}, {"pageSetup", setup}, {"job", job}})
                  .toJson(QJsonDocument::Compact);
    if (quit_) {
        std::fputs((answer_ + '\n').constData(), stdout);
        std::fflush(stdout);
    }
    finish();
}

void PrintPrompt::cancel() {
    // Nothing on stdout: the portal tells the app it was cancelled.
    finish();
}

void PrintPrompt::finish() {
    answered_ = true;
    if (quit_)
        QCoreApplication::quit();
}

} // namespace atrium
