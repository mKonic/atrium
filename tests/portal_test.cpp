#include "portal_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::portal;

TEST(Portal, MailtoCarriesEveryField) {
    Email e{.to = {"a@example.org", "b@example.org"}, .cc = {"c@example.org"}, .bcc = {"d@example.org"},
            .subject = "Hello there", .body = "Line 1\nä & more?", .has_subject = true, .has_body = true};
    EXPECT_EQ(mailto(e), "mailto:a@example.org,b@example.org?cc=c@example.org&bcc=d@example.org"
                         "&subject=Hello%20there&body=Line%201%0A%C3%A4%20%26%20more%3F");
}

TEST(Portal, MailtoLeavesOutWhatIsntAsked) {
    EXPECT_EQ(mailto({}), "mailto:");
    EXPECT_EQ(mailto({.to = {"a@b"}}), "mailto:a@b");
    // Asked for, but empty: still there, as the app said.
    EXPECT_EQ(mailto({.has_subject = true}), "mailto:?subject=");
    EXPECT_EQ(mailto({.cc = {"", "x@y"}, .has_body = true}), "mailto:?cc=x@y&body=");
    // An address can't smuggle in another field.
    EXPECT_EQ(mailto({.to = {"a@b?bcc=evil@x"}}), "mailto:a@b%3Fbcc%3Devil@x");
}

TEST(Portal, OnlyUrgentIsCritical) {
    EXPECT_EQ(urgency("low"), 0);
    EXPECT_EQ(urgency("normal"), 1);
    EXPECT_EQ(urgency("high"), 1);
    EXPECT_EQ(urgency(""), 1);
    EXPECT_EQ(urgency("urgent"), 2);
}
