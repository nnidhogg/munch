#include "munch/regex/set.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>

#include "munch/nfa/tools/graphviz.hpp"

using namespace munch;
using namespace munch::regex;
using namespace munch::nfa::tools;

class Set_test : public testing::Test
{
protected:
    Set empty_set;
    Set digits_set;
    Set alpha_set;
    Set alphanum_set;

    void SetUp() override
    {
        digits_set = Set::digits();
        alpha_set = Set::alpha();
        alphanum_set = Set::alphanum();
    }

    void write_dot(const auto& nfa, const std::string& name) const
    {
        Graphviz::to_file(nfa, debug_path_ / (name + ".dot"));
    }

private:
    std::filesystem::path debug_path_{std::string(SOURCE_DIR) + "/debug/"};
};

TEST_F(Set_test, Constructor_empty)
{
    EXPECT_TRUE(empty_set.symbols().empty());
}

TEST_F(Set_test, Constructor_initializer_list)
{
    const Set s = {'a', 'b', 'c'};
    EXPECT_EQ(s.symbols().size(), 3);
    EXPECT_TRUE(s.symbols().contains('a'));
    EXPECT_TRUE(s.symbols().contains('b'));
    EXPECT_TRUE(s.symbols().contains('c'));
}

TEST_F(Set_test, Constructor_from_existing_symbols)
{
    const Set::Symbols_t symbols{'a', 'b', 'c'};
    const Set s{symbols};

    EXPECT_EQ(s.symbols(), symbols);
}

TEST_F(Set_test, From_char)
{
    const Set s = Set::from('x');
    EXPECT_EQ(s.symbols().size(), 1);
    EXPECT_TRUE(s.symbols().contains('x'));
}

TEST_F(Set_test, From_chars)
{
    const Set s = Set::from({'a', 'b', 'c'});
    EXPECT_EQ(s.symbols().size(), 3);
    EXPECT_TRUE(s.symbols().contains('a'));
    EXPECT_TRUE(s.symbols().contains('b'));
    EXPECT_TRUE(s.symbols().contains('c'));
}

TEST_F(Set_test, From_range)
{
    const Set s = Set::range('a', 'c');
    EXPECT_EQ(s.symbols().size(), 3);
    EXPECT_TRUE(s.symbols().contains('a'));
    EXPECT_TRUE(s.symbols().contains('b'));
    EXPECT_TRUE(s.symbols().contains('c'));
}

TEST_F(Set_test, Range_crossing_high_bit)
{
    const auto low = static_cast<char>(static_cast<unsigned char>(0x7E));
    const auto high = static_cast<char>(static_cast<unsigned char>(0x81));
    const Set s = Set::range(low, high);
    EXPECT_EQ(s.symbols().size(), 4);
    for (unsigned i = 0x7E; i <= 0x81; ++i)
    {
        EXPECT_TRUE(s.symbols().contains(static_cast<char>(static_cast<unsigned char>(i))));
    }
}

TEST_F(Set_test, Range_rejects_a_reversed_range)
{
    // A single-symbol range is the boundary and must still be accepted.
    EXPECT_EQ(Set::range('a', 'a').symbols().size(), 1);

    // One past it in the wrong direction is an error rather than an empty set: views::iota would be undefined here,
    // and a silently empty result would hide the typo.
    EXPECT_THROW((void)Set::range('9', '0'), std::invalid_argument);
    EXPECT_THROW((void)Set::range('b', 'a'), std::invalid_argument);

    // Ordering is by unsigned byte, so a high-bit symbol orders after an ASCII one rather than before it.
    const auto high{static_cast<char>(static_cast<unsigned char>(0x80))};

    EXPECT_NO_THROW((void)Set::range('a', high));
    EXPECT_THROW((void)Set::range(high, 'a'), std::invalid_argument);
}

TEST_F(Set_test, Range_ending_at_max_byte)
{
    const auto low = static_cast<char>(static_cast<unsigned char>(0xFE));
    const auto high = static_cast<char>(static_cast<unsigned char>(0xFF));
    const Set s = Set::range(low, high);
    EXPECT_EQ(s.symbols().size(), 2);
    EXPECT_TRUE(s.symbols().contains(static_cast<char>(static_cast<unsigned char>(0xFE))));
    EXPECT_TRUE(s.symbols().contains(static_cast<char>(static_cast<unsigned char>(0xFF))));
}

TEST_F(Set_test, Digits)
{
    EXPECT_EQ(digits_set.symbols().size(), 10);
    for (char c = '0'; c <= '9'; ++c)
    {
        EXPECT_TRUE(digits_set.symbols().contains(c));
    }
}

TEST_F(Set_test, Alpha)
{
    EXPECT_EQ(alpha_set.symbols().size(), 52);
    for (char c = 'a'; c <= 'z'; ++c)
    {
        EXPECT_TRUE(alpha_set.symbols().contains(c));
    }
    for (char c = 'A'; c <= 'Z'; ++c)
    {
        EXPECT_TRUE(alpha_set.symbols().contains(c));
    }
}

TEST_F(Set_test, Alphanum)
{
    EXPECT_EQ(alphanum_set.symbols().size(), 62);
    for (char c = 'a'; c <= 'z'; ++c)
    {
        EXPECT_TRUE(alphanum_set.symbols().contains(c));
    }
    for (char c = 'A'; c <= 'Z'; ++c)
    {
        EXPECT_TRUE(alphanum_set.symbols().contains(c));
    }
    for (char c = '0'; c <= '9'; ++c)
    {
        EXPECT_TRUE(alphanum_set.symbols().contains(c));
    }
}

TEST_F(Set_test, Printable)
{
    const Set s = Set::printable();
    EXPECT_EQ(s.symbols().size(), 95);
    for (char c = ' '; c <= '~'; ++c)
    {
        EXPECT_TRUE(s.symbols().contains(c));
    }
}

TEST_F(Set_test, All)
{
    const Set s = Set::all();
    EXPECT_EQ(s.symbols().size(), 256);
    for (int i = 0; i <= 255; ++i)
    {
        EXPECT_TRUE(s.symbols().contains(static_cast<char>(static_cast<unsigned char>(i))));
    }
}

TEST_F(Set_test, Operator_plus_equal_set)
{
    Set s = Set::from('x');
    s += Set::from('y');
    EXPECT_EQ(s.symbols().size(), 2);
    EXPECT_TRUE(s.symbols().contains('x'));
    EXPECT_TRUE(s.symbols().contains('y'));
}

TEST_F(Set_test, Operator_plus_equal_char)
{
    Set s = Set::from('x');
    s += 'y';
    EXPECT_EQ(s.symbols().size(), 2);
    EXPECT_TRUE(s.symbols().contains('x'));
    EXPECT_TRUE(s.symbols().contains('y'));
}

TEST_F(Set_test, Operator_plus_equal_initializer_list_chars)
{
    Set s = Set::from('x');
    s += {'y', 'z'};
    EXPECT_EQ(s.symbols().size(), 3);
    EXPECT_TRUE(s.symbols().contains('x'));
    EXPECT_TRUE(s.symbols().contains('y'));
    EXPECT_TRUE(s.symbols().contains('z'));
}

TEST_F(Set_test, Operator_plus_set)
{
    const Set s1 = Set::from('x');
    const Set s2 = Set::from('y');
    const Set s = s1 + s2;
    EXPECT_EQ(s.symbols().size(), 2);
    EXPECT_TRUE(s.symbols().contains('x'));
    EXPECT_TRUE(s.symbols().contains('y'));
}

TEST_F(Set_test, Operator_plus_char)
{
    const Set s = Set::from('x') + 'y';
    EXPECT_EQ(s.symbols().size(), 2);
    EXPECT_TRUE(s.symbols().contains('x'));
    EXPECT_TRUE(s.symbols().contains('y'));
}

TEST_F(Set_test, Operator_minus_equal_set)
{
    Set s = Set::from({'a', 'b', 'c', 'd'});
    s -= Set::from({'b', 'c'});
    EXPECT_EQ(s.symbols().size(), 2);
    EXPECT_TRUE(s.symbols().contains('a'));
    EXPECT_TRUE(s.symbols().contains('d'));
}

TEST_F(Set_test, Operator_minus_equal_char)
{
    Set s = Set::from({'a', 'b', 'c'});
    s -= 'b';
    EXPECT_EQ(s.symbols().size(), 2);
    EXPECT_TRUE(s.symbols().contains('a'));
    EXPECT_TRUE(s.symbols().contains('c'));
}

TEST_F(Set_test, Operator_minus_equal_initializer_list_chars)
{
    Set s = Set::from({'a', 'b', 'c', 'd'});
    s -= {'b', 'c'};
    EXPECT_EQ(s.symbols().size(), 2);
    EXPECT_TRUE(s.symbols().contains('a'));
    EXPECT_TRUE(s.symbols().contains('d'));
}

TEST_F(Set_test, Operator_minus_set)
{
    const Set s = Set::from({'a', 'b', 'c', 'd'});
    const Set result = s - Set::from({'b', 'c'});
    EXPECT_EQ(result.symbols().size(), 2);
    EXPECT_TRUE(result.symbols().contains('a'));
    EXPECT_TRUE(result.symbols().contains('d'));
}

TEST_F(Set_test, Operator_minus_char)
{
    const Set s = Set::from({'a', 'b', 'c'});
    const Set result = s - 'b';
    EXPECT_EQ(result.symbols().size(), 2);
    EXPECT_TRUE(result.symbols().contains('a'));
    EXPECT_TRUE(result.symbols().contains('c'));
}

TEST_F(Set_test, Operator_plus_commute_char_set)
{
    const Set s = 'x' + Set::from('y');
    EXPECT_EQ(s.symbols().size(), 2);
    EXPECT_TRUE(s.symbols().contains('x'));
    EXPECT_TRUE(s.symbols().contains('y'));
}

TEST_F(Set_test, Operator_plus_overlapping)
{
    const Set s1 = Set::from({'a', 'b', 'c'});
    const Set s2 = Set::from({'b', 'c', 'd'});
    const Set s = s1 + s2;
    EXPECT_EQ(s.symbols().size(), 4);
    EXPECT_TRUE(s.symbols().contains('a'));
    EXPECT_TRUE(s.symbols().contains('b'));
    EXPECT_TRUE(s.symbols().contains('c'));
    EXPECT_TRUE(s.symbols().contains('d'));
}

TEST_F(Set_test, Operator_minus_non_existing)
{
    Set s = {'a', 'b', 'c'};
    s -= 'd'; // 'd' is not in the set
    EXPECT_EQ(s.symbols().size(), 3);
    EXPECT_TRUE(s.symbols().contains('a'));
    EXPECT_TRUE(s.symbols().contains('b'));
    EXPECT_TRUE(s.symbols().contains('c'));
}

TEST_F(Set_test, Operator_minus_all_elements)
{
    Set s = {'a', 'b', 'c'};
    s -= Set::from({'a', 'b', 'c'});
    EXPECT_TRUE(s.symbols().empty());
}

TEST_F(Set_test, Large_set)
{
    Set s = Set::all();
    EXPECT_EQ(s.symbols().size(), 256);
    s -= Set::printable();
    EXPECT_EQ(s.symbols().size(), 161); // 256 - 95 = 161
}

TEST_F(Set_test, Copy_constructor)
{
    const Set s1 = Set::from({'a', 'b', 'c'});
    const Set s2(s1);
    EXPECT_EQ(s1.symbols(), s2.symbols());
}

TEST_F(Set_test, Copy_assignment)
{
    const Set s1 = Set::from({'a', 'b', 'c'});
    Set s2;
    s2 = s1;
    EXPECT_EQ(s1.symbols(), s2.symbols());
}

TEST_F(Set_test, Move_constructor)
{
    Set s1 = {'a', 'b', 'c'};
    const Set s2(std::move(s1));
    EXPECT_EQ(s2.symbols().size(), 3);
}

TEST_F(Set_test, Move_assignment)
{
    Set s1 = {'a', 'b', 'c'};
    Set s2;
    s2 = std::move(s1);
    EXPECT_EQ(s2.symbols().size(), 3);
}
