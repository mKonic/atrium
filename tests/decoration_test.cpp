#include "decoration_core.hpp"

#include <gtest/gtest.h>

using atrium::DecorationLatch;

TEST(DecorationLatch, NothingDroppedNothingToDo) {
    DecorationLatch d;
    EXPECT_FALSE(d.commit(true));
}

// Qt's close: the decoration object, then the window, with no commit between.
// The frame keeps its title bar to the end, so the size remembered has it.
TEST(DecorationLatch, DroppedOnTheWayOutNeverRestyles) {
    DecorationLatch d;
    d.dropped();
    EXPECT_FALSE(d.commit(false));  // the commit that unmaps it
    d.unmapped();
    // Mapped again later (a window that hides and shows): nothing left over.
    EXPECT_FALSE(d.commit(true));
}

// A window that drops the object and carries on draws its own title bar from
// its next commit, and ours goes then, once.
TEST(DecorationLatch, DroppedAndCarryingOnRestylesAtTheNextCommit) {
    DecorationLatch d;
    d.dropped();
    EXPECT_TRUE(d.commit(true));
    EXPECT_FALSE(d.commit(true));
}
