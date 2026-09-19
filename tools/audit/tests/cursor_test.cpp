#include "munch/tools/audit/cursor.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string_view>

#include "munch/tools/audit/lexer_spec.hpp"

using namespace munch::tools::audit;

TEST(Cursor, Done_is_the_end_of_the_span_and_skip_blanks_stops_at_the_first_byte_that_is_neither_blank_nor_comment)
{
    // The span form ends where it says, not where the text does: one byte to take, then done, with
    // the text still holding more.
    Cursor span{"abc", 1, 2};

    EXPECT_FALSE(span.done());
    EXPECT_EQ(span.next("a byte"), 'b');
    EXPECT_TRUE(span.done());

    EXPECT_TRUE(Cursor{""}.done());

    // Every blank kind and both comment styles in one run, the block comment spanning a line, so the byte reached
    // and the line it is on are both pinned; the line is counted from one by the newlines passed.
    Cursor blanks{" \t\r\n// a line comment\n/* a block\n comment */ \tx"};

    blanks.skip_blanks();

    EXPECT_EQ(blanks.peek(), std::optional{'x'});
    EXPECT_EQ(blanks.line(), 4U);
    EXPECT_FALSE(blanks.done());

    // Nothing to skip leaves the cursor where it was, a lone slash is no comment, and an ended span is
    // skipped over without a refusal.
    Cursor slash{"/x"};

    slash.skip_blanks();

    EXPECT_EQ(slash.offset(), 0U);

    Cursor ended{"  "};

    ended.skip_blanks();

    EXPECT_TRUE(ended.done());

    ended.skip_blanks();

    EXPECT_TRUE(ended.done());

    // A block comment that never closes is refused at the line it opens on, and one closing beyond the span's end
    // is as unclosed as one closing nowhere.
    Cursor open{"\n\n/* never"};

    EXPECT_THROW(open.skip_blanks(), Spec_error);

    Cursor beyond{"/* closes */ x", 0, 5};

    EXPECT_THROW(beyond.skip_blanks(), Spec_error);
}
