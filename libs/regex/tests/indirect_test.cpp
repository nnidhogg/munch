#include "munch/regex/indirect.hpp"

#include <gtest/gtest.h>

#include <utility>

using namespace munch::regex;

TEST(Indirect_test, Copy_construction_copies_the_value)
{
    const Indirect<int> original{1};

    Indirect<int> copy{original};

    *copy = 2;

    EXPECT_EQ(*original, 1);
    EXPECT_EQ(*copy, 2);
}

TEST(Indirect_test, Copy_assignment_copies_the_value)
{
    const Indirect<int> original{1};

    Indirect<int> target{2};

    target = original;

    *target = 3;

    EXPECT_EQ(*original, 1);
    EXPECT_EQ(*target, 3);
}

TEST(Indirect_test, Self_assignment_keeps_the_value)
{
    Indirect<int> box{1};

    // Through a reference, so the self-assignment is not visible to compiler warnings about trivial `x = x`.
    auto& same{box};

    box = same;

    EXPECT_EQ(*box, 1);
}

TEST(Indirect_test, Move_construction_transfers_the_value)
{
    Indirect<int> source{1};

    const Indirect<int> target{std::move(source)};

    // A moved-from Indirect may only be assigned to or destroyed; assigning revives it.
    EXPECT_EQ(*target, 1);

    source = Indirect<int>{2};

    EXPECT_EQ(*source, 2);
}

TEST(Indirect_test, Move_assignment_transfers_the_value)
{
    Indirect<int> source{1};

    Indirect<int> target{2};

    target = std::move(source);

    EXPECT_EQ(*target, 1);
}
