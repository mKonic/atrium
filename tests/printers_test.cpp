#include "printers_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::printers;

TEST(Printers, ParsesLpstat) {
    const char* text = "printer Office is idle.  enabled since Thu 24 Sep 2026 10:00:00 AM UTC\n"
                       "\tForm mounted:\n\tDescription: HP LaserJet Pro\n\tLocation: 2nd floor\n"
                       "printer Home-Laser now printing Home-Laser-7.  enabled since Thu 24 Sep 2026\n"
                       "\tDescription: Brother\n"
                       "printer Old disabled since Wed 23 Sep 2026 -\n\treason unknown\n";
    const auto p = parse_printers(text);
    ASSERT_EQ(p.size(), 3u);
    EXPECT_EQ(p[0].name, "Office");
    EXPECT_EQ(p[0].description, "HP LaserJet Pro");
    EXPECT_EQ(p[0].state, "idle");
    EXPECT_EQ(p[1].state, "printing");
    EXPECT_EQ(p[2].state, "stopped");
    EXPECT_FALSE(p[2].enabled);
}

TEST(Printers, Default) {
    EXPECT_EQ(parse_default("system default destination: Office\n"), "Office");
    EXPECT_EQ(parse_default("no system default destination\n"), "");
}

TEST(Printers, JobsByTheirPrinter) {
    const std::vector<Printer> printers = {{"Home"}, {"Home-Laser"}};
    const auto jobs = parse_jobs("Home-Laser-7  alice  2048  Thu 24 Sep 2026\nHome-3 bob 10 Thu\njunk\n", printers);
    ASSERT_EQ(jobs.size(), 2u);
    EXPECT_EQ(jobs[0].printer, "Home-Laser");
    EXPECT_EQ(jobs[0].user, "alice");
    EXPECT_EQ(jobs[0].size, 2048);
    EXPECT_EQ(jobs[1].printer, "Home");
}

TEST(Printers, ParsesOptions) {
    const auto o = parse_options("PageSize/Media Size: *Letter Legal A4 Env10\n"
                                 "Duplex/2-Sided Printing: *None DuplexNoTumble DuplexTumble\n"
                                 "ColorModel/Color Mode: Gray *RGB\n"
                                 "Resolution: 300dpi *600dpi\n"
                                 "garbage line\n");
    ASSERT_EQ(o.size(), 4u);
    EXPECT_EQ(o[0].key, "PageSize");
    EXPECT_EQ(o[0].label, "Media Size");
    EXPECT_EQ(o[0].choices, (std::vector<std::string>{"Letter", "Legal", "A4", "Env10"}));
    EXPECT_EQ(o[0].current, "Letter");
    EXPECT_EQ(o[1].current, "None");
    EXPECT_EQ(o[2].current, "RGB");
    EXPECT_EQ(o[3].label, "Resolution");  // no label of its own: its key
    EXPECT_EQ(o[3].current, "600dpi");
}

TEST(Printers, Papers) {
    ASSERT_NE(paper_by_ppd("A4"), nullptr);
    EXPECT_EQ(paper_by_ppd("A4")->pwg, "iso_a4");
    EXPECT_EQ(paper_by_ppd("A4")->width, 210);
    EXPECT_EQ(paper_by_pwg("na_letter")->ppd, "Letter");
    EXPECT_EQ(paper_by_ppd("Custom.10x10"), nullptr);
}

TEST(Printers, LpArguments) {
    EXPECT_EQ(lp_args({.printer = "Office"}), (std::vector<std::string>{"-d", "Office"}));
    PrintJob j{.printer = "Office", .title = "-report", .copies = 2, .ranges = "1-3,5", .page_set = "odd",
               .paper = "A4", .landscape = true, .duplex = "DuplexNoTumble", .color_model = "Gray",
               .collate = false, .reverse = true, .number_up = 2};
    EXPECT_EQ(lp_args(j), (std::vector<std::string>{"-d", "Office", "-t", "-report", "-n", "2", "-P", "1-3,5",
                                                    "-o", "page-set=odd", "-o", "PageSize=A4", "-o", "landscape",
                                                    "-o", "Duplex=DuplexNoTumble", "-o", "ColorModel=Gray",
                                                    "-o", "collate=false", "-o", "outputorder=reverse",
                                                    "-o", "number-up=2"}));
}

TEST(Printers, PageRangesFromOne) {
    EXPECT_EQ(zero_based_ranges("1-3, 5"), "0-2,4");
    EXPECT_EQ(zero_based_ranges(" 7 "), "6");
    EXPECT_EQ(zero_based_ranges("2-2,"), "1");
    EXPECT_EQ(zero_based_ranges(""), std::nullopt);
    EXPECT_EQ(zero_based_ranges("0"), std::nullopt);
    EXPECT_EQ(zero_based_ranges("5-3"), std::nullopt);
    EXPECT_EQ(zero_based_ranges("a-b"), std::nullopt);
    EXPECT_EQ(zero_based_ranges("1--3"), std::nullopt);
}
