#include "print_prompt.hpp"
#include "qt_test.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using atrium::PrintPrompt;

namespace {

// CUPS as two printers: an office laser with both sides and colour, and a
// label printer with neither.
void cups(const QString& program, const QStringList& args, std::function<void(const QByteArray&)> done) {
    if (program == "lpstat" && args == QStringList{"-l", "-p"})
        done("printer Office is idle.  enabled since Mon\n\tDescription: Office Laser\n"
             "printer Labels is idle.  enabled since Mon\n");
    else if (program == "lpstat" && args == QStringList{"-d"})
        done("system default destination: Labels\n");
    else if (program == "lpoptions" && args.value(1) == "Office")
        done("PageSize/Media Size: Custom.WIDTHxHEIGHT Letter *A4 A5\nDuplex/2-Sided Printing: *None DuplexNoTumble DuplexTumble\n"
             "ColorModel/Color Mode: Gray *RGB\n");
    else if (program == "lpoptions")
        done("PageSize/Media Size: *4x6\n");
    else
        done({});
}

QJsonObject answered(const PrintPrompt& p) {
    return QJsonDocument::fromJson(p.answer()).object();
}

} // namespace

TEST(PrintPrompt, StartsWhereTheAppLeftOff) {
    PrintPrompt p(R"({"title":"Report","settings":{"printer":"Office","n-copies":"3","paper-format":"iso_a5",)"
                  R"("duplex":"horizontal","use-color":"false","orientation":"landscape"}})",
                  cups);
    EXPECT_EQ(p.printers().size(), 3);  // two, and Save as PDF
    EXPECT_EQ(p.printer(), "Office");
    EXPECT_EQ(p.copies(), 3);
    EXPECT_EQ(p.paper(), "A5");
    EXPECT_EQ(p.papers().size(), 3);  // not the custom size's placeholder
    EXPECT_EQ(p.duplex(), "DuplexNoTumble");
    EXPECT_EQ(p.color(), "Gray");
    EXPECT_TRUE(p.landscape());
    EXPECT_EQ(p.acceptLabel(), "Print");
    // Another printer: its own paper, and nothing to say about sides or colour.
    p.setPrinter("Labels");
    EXPECT_EQ(p.paper(), "4x6");
    EXPECT_TRUE(p.duplexes().isEmpty());
    EXPECT_TRUE(p.colors().isEmpty());
}

TEST(PrintPrompt, TheDefaultPrinterOtherwise) {
    PrintPrompt p("{}", cups);
    EXPECT_EQ(p.printer(), "Labels");
}

TEST(PrintPrompt, AnswersSettingsTheAppRendersBy) {
    PrintPrompt p(R"({"title":"Report","settings":{"printer":"Office"}})", cups);
    p.setPages("2-3, 5");
    p.setCopies(2);
    p.setDuplex("DuplexTumble");
    p.print();
    const QJsonObject a = answered(p);
    const QJsonObject s = a.value("settings").toObject();
    EXPECT_EQ(s.value("print-pages"), "ranges");
    EXPECT_EQ(s.value("page-ranges"), "1-2,4");  // GTK counts from 0
    EXPECT_EQ(s.value("n-copies"), "2");
    EXPECT_EQ(s.value("duplex"), "vertical");
    EXPECT_EQ(s.value("paper-format"), "iso_a4");
    EXPECT_EQ(a.value("pageSetup").toObject().value("Width").toDouble(), 210);
    EXPECT_EQ(a.value("job").toObject().value("printer"), "Office");
    EXPECT_EQ(a.value("job").toObject().value("duplex"), "DuplexTumble");
}

TEST(PrintPrompt, ProblemsStopIt) {
    PrintPrompt p(R"({"settings":{"printer":"Office"}})", cups);
    p.setPages("3-1");
    EXPECT_FALSE(p.problem().isEmpty());
    p.print();
    EXPECT_TRUE(p.answer().isEmpty());
    p.setPages("");
    p.setCopies(0);
    EXPECT_FALSE(p.problem().isEmpty());
    p.setCopies(1);
    EXPECT_TRUE(p.problem().isEmpty());
}

TEST(PrintPrompt, SavesAsPdf) {
    QTemporaryDir dir;
    PrintPrompt p(R"({"title":"a/b","settings":{"output-uri":"file:///x.pdf"}})", cups);
    EXPECT_TRUE(p.pdf());
    EXPECT_TRUE(p.pdfFile().endsWith("/a-b.pdf"));
    EXPECT_GT(p.papers().size(), 10);
    p.setPdfFile(dir.filePath("out.pdf"));
    p.print();
    const QJsonObject a = answered(p);
    EXPECT_EQ(a.value("settings").toObject().value("output-uri"), "file://" + dir.filePath("out.pdf"));
    EXPECT_EQ(a.value("settings").toObject().value("printer"), "Print to File");
    EXPECT_EQ(a.value("job").toObject().value("pdf"), dir.filePath("out.pdf"));
}
