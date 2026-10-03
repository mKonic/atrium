#pragma once
// The print dialog (print.qml), run by atrium-portal's Print when an app
// asks to print: which printer (or a PDF), how many copies, which pages, the
// paper, its orientation, both sides and colour where the printer can. The
// request is JSON on stdin, the answer JSON on stdout.
//
//   asked:    { title, accept, settings: { GTK print settings }, pageSetup }
//   answered: { settings, pageSetup, job: { printer, pdf, copies, paper,
//               duplex, color } }   (nothing when cancelled)
//
// settings and pageSetup are what the app renders by (GTK's names); job is
// what atrium-portal sends to CUPS with the document the app then hands it.

#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

namespace atrium {

class PrintPrompt : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QString acceptLabel READ acceptLabel CONSTANT)
    // [{ value, label }]: the printers, then "Save as PDF" (value "pdf").
    Q_PROPERTY(QVariantList printers READ printers NOTIFY printersChanged)
    Q_PROPERTY(QString printer READ printer WRITE setPrinter NOTIFY printerChanged)
    Q_PROPERTY(bool pdf READ pdf NOTIFY printerChanged)
    // The chosen printer's papers, sides and colours ([{ value, label }];
    // none: it has no say).
    Q_PROPERTY(QVariantList papers READ papers NOTIFY optionsChanged)
    Q_PROPERTY(QVariantList duplexes READ duplexes NOTIFY optionsChanged)
    Q_PROPERTY(QVariantList colors READ colors NOTIFY optionsChanged)
    Q_PROPERTY(QString paper READ paper WRITE setPaper NOTIFY changed)
    Q_PROPERTY(QString duplex READ duplex WRITE setDuplex NOTIFY changed)
    Q_PROPERTY(QString color READ color WRITE setColor NOTIFY changed)
    Q_PROPERTY(int copies READ copies WRITE setCopies NOTIFY changed)
    Q_PROPERTY(QString pages READ pages WRITE setPages NOTIFY changed)  // "1-3, 5"; "": all
    Q_PROPERTY(bool landscape READ landscape WRITE setLandscape NOTIFY changed)
    Q_PROPERTY(QString pdfFile READ pdfFile WRITE setPdfFile NOTIFY changed)
    // What stops printing ("" when nothing does).
    Q_PROPERTY(QString problem READ problem NOTIFY changed)

public:
    // Runs a program and hands back its output (stdout; empty on failure).
    using Run = std::function<void(const QString& program, const QStringList& args,
                                   std::function<void(const QByteArray&)> done)>;

    explicit PrintPrompt(QObject* parent = nullptr);  // stdin, and the real lpstat
    PrintPrompt(const QByteArray& request, Run run, QObject* parent = nullptr);

    QString title() const { return title_; }
    QString acceptLabel() const { return accept_; }
    QVariantList printers() const { return printers_; }
    QString printer() const { return printer_; }
    void setPrinter(const QString& name);
    bool pdf() const { return printer_ == "pdf"; }
    QVariantList papers() const { return papers_; }
    QVariantList duplexes() const { return duplexes_; }
    QVariantList colors() const { return colors_; }
    QString paper() const { return paper_; }
    void setPaper(const QString& v);
    QString duplex() const { return duplex_; }
    void setDuplex(const QString& v);
    QString color() const { return color_; }
    void setColor(const QString& v);
    int copies() const { return copies_; }
    void setCopies(int n);
    QString pages() const { return pages_; }
    void setPages(const QString& v);
    bool landscape() const { return landscape_; }
    void setLandscape(bool on);
    QString pdfFile() const { return pdfFile_; }
    void setPdfFile(const QString& v);
    QString problem() const;

    Q_INVOKABLE void print();
    Q_INVOKABLE void cancel();

    QByteArray answer() const { return answer_; }  // tests

signals:
    void printersChanged();
    void printerChanged();
    void optionsChanged();
    void changed();

private:
    void read(const QByteArray& request);
    void loadPrinters();
    void loadOptions();
    void finish();

    Run run_;
    QString title_, accept_;
    QVariantMap asked_;  // the app's settings
    QVariantList printers_, papers_, duplexes_, colors_;
    QString printer_, paper_, duplex_, color_, pages_, pdfFile_;
    int copies_ = 1;
    bool landscape_ = false;
    QByteArray answer_;
    bool answered_ = false, quit_ = true;
};

} // namespace atrium
