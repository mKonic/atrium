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
