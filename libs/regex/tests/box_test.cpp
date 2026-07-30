#include "munch/regex/box.hpp"

#include <gtest/gtest.h>

#include <utility>

using namespace munch::regex;

TEST(Box_test, Copy_construction_copies_the_value)
{
    const Box<int> original{1};

    Box<int> copy{original};

    *copy = 2;

    EXPECT_EQ(*original, 1);
    EXPECT_EQ(*copy, 2);
}

TEST(Box_test, Copy_assignment_copies_the_value)
{
    const Box<int> original{1};

    Box<int> target{2};

    target = original;

    *target = 3;

    EXPECT_EQ(*original, 1);
    EXPECT_EQ(*target, 3);
}

TEST(Box_test, Self_assignment_keeps_the_value)
{
    Box<int> box{1};

    // Through a reference, so the self-assignment is not visible to compiler warnings about trivial `x = x`.
    auto& same{box};

    box = same;

    EXPECT_EQ(*box, 1);
}

TEST(Box_test, Move_construction_transfers_the_value)
{
    Box<int> source{1};

    const Box<int> target{std::move(source)};

    // A moved-from Box may only be assigned to or destroyed; assigning revives it.
    EXPECT_EQ(*target, 1);

    source = Box<int>{2};

    EXPECT_EQ(*source, 2);
}

TEST(Box_test, Move_assignment_transfers_the_value)
{
    Box<int> source{1};

    Box<int> target{2};

    target = std::move(source);

    EXPECT_EQ(*target, 1);
}
