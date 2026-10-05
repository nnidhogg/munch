#include "munch/regex/set.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <tuple>
#include <utility>

using namespace munch::regex;

namespace
{
/**
 * @brief Returns the char holding a byte value.
 * @param value The byte value, 0 to 255.
 * @return The byte as a char.
 */
constexpr char byte(const unsigned value)
{
    return static_cast<char>(static_cast<unsigned char>(value));
}

} // namespace

TEST(Set_test, A_default_set_is_empty)
{
    const Set empty_set{};

    EXPECT_TRUE(empty_set.symbols().empty());
}

TEST(Set_test, An_initializer_list_set_holds_its_symbols)
{
    const Set set{'a', 'b', 'c'};

    EXPECT_EQ(set.symbols().size(), 3U);
    EXPECT_TRUE(set.symbols().contains('a'));
    EXPECT_TRUE(set.symbols().contains('b'));
    EXPECT_TRUE(set.symbols().contains('c'));
}

TEST(Set_test, A_set_built_from_symbols_holds_exactly_them)
{
    const Set::Symbols_t symbols{'a', 'b', 'c'};

    const Set set{symbols};

    EXPECT_EQ(set.symbols(), symbols);
}

TEST(Set_test, From_a_symbol_holds_that_symbol_alone)
{
    const Set set{Set::from('x')};

    EXPECT_EQ(set.symbols().size(), 1U);
    EXPECT_TRUE(set.symbols().contains('x'));
}

TEST(Set_test, From_a_list_holds_every_listed_symbol)
{
    const Set set{Set::from({'a', 'b', 'c'})};

    EXPECT_EQ(set.symbols().size(), 3U);
    EXPECT_TRUE(set.symbols().contains('a'));
    EXPECT_TRUE(set.symbols().contains('b'));
    EXPECT_TRUE(set.symbols().contains('c'));
}

TEST(Set_test, A_range_holds_both_ends_and_everything_between)
{
    const Set set{Set::range('a', 'c')};

    EXPECT_EQ(set.symbols().size(), 3U);
    EXPECT_TRUE(set.symbols().contains('a'));
    EXPECT_TRUE(set.symbols().contains('b'));
    EXPECT_TRUE(set.symbols().contains('c'));
}

TEST(Set_test, A_range_crosses_the_high_bit_in_unsigned_order)
{
    const auto low{byte(0x7E)};

    const auto high{byte(0x81)};

    const Set set{Set::range(low, high)};

    EXPECT_EQ(set.symbols().size(), 4U);

    for (unsigned value{0x7E}; value <= 0x81; ++value)
    {
        EXPECT_TRUE(set.symbols().contains(byte(value)));
    }
}

TEST(Set_test, A_reversed_range_throws)
{
    // A single-symbol range is the boundary and must still be accepted.
    EXPECT_EQ(Set::range('a', 'a').symbols().size(), 1U);

    // One past it in the wrong direction is an error rather than an empty set: views::iota would be undefined here, and
    // a silently empty result would hide the typo.
    EXPECT_THROW(std::ignore = Set::range('9', '0'), std::invalid_argument);
    EXPECT_THROW(std::ignore = Set::range('b', 'a'), std::invalid_argument);

    // Ordering is by unsigned byte, so a high-bit symbol orders after an ASCII one rather than before it.
    const auto high{byte(0x80)};

    EXPECT_NO_THROW(std::ignore = Set::range('a', high));
    EXPECT_THROW(std::ignore = Set::range(high, 'a'), std::invalid_argument);
}

TEST(Set_test, A_range_reaches_the_largest_byte)
{
    const auto low{byte(0xFE)};

    const auto high{byte(0xFF)};

    const Set set{Set::range(low, high)};

    EXPECT_EQ(set.symbols().size(), 2U);
    EXPECT_TRUE(set.symbols().contains(low));
    EXPECT_TRUE(set.symbols().contains(high));
}

TEST(Set_test, Digits_are_the_ten_decimal_digits)
{
    const auto digits_set{Set::digits()};

    EXPECT_EQ(digits_set.symbols().size(), 10U);

    for (char symbol{'0'}; symbol <= '9'; ++symbol)
    {
        EXPECT_TRUE(digits_set.symbols().contains(symbol));
    }
}

TEST(Set_test, Alpha_is_the_fifty_two_ascii_letters)
{
    const auto alpha_set{Set::alpha()};

    EXPECT_EQ(alpha_set.symbols().size(), 52U);

    for (char symbol{'a'}; symbol <= 'z'; ++symbol)
    {
        EXPECT_TRUE(alpha_set.symbols().contains(symbol));
    }

    for (char symbol{'A'}; symbol <= 'Z'; ++symbol)
    {
        EXPECT_TRUE(alpha_set.symbols().contains(symbol));
    }
}

TEST(Set_test, Alphanum_is_the_letters_and_the_digits)
{
    const auto alphanum_set{Set::alphanum()};

    EXPECT_EQ(alphanum_set.symbols().size(), 62U);

    for (char symbol{'a'}; symbol <= 'z'; ++symbol)
    {
        EXPECT_TRUE(alphanum_set.symbols().contains(symbol));
    }

    for (char symbol{'A'}; symbol <= 'Z'; ++symbol)
    {
        EXPECT_TRUE(alphanum_set.symbols().contains(symbol));
    }

    for (char symbol{'0'}; symbol <= '9'; ++symbol)
    {
        EXPECT_TRUE(alphanum_set.symbols().contains(symbol));
    }
}

TEST(Set_test, Printable_is_the_blank_through_the_tilde)
{
    const Set set{Set::printable()};

    EXPECT_EQ(set.symbols().size(), 95U);

    for (char symbol{' '}; symbol <= '~'; ++symbol)
    {
        EXPECT_TRUE(set.symbols().contains(symbol));
    }
}

TEST(Set_test, All_holds_every_byte)
{
    const Set set{Set::all()};

    EXPECT_EQ(set.symbols().size(), 256U);

    for (unsigned value{0}; value <= 255; ++value)
    {
        EXPECT_TRUE(set.symbols().contains(byte(value)));
    }
}

TEST(Set_test, Adding_a_set_adds_its_symbols)
{
    Set set{Set::from('x')};

    set += Set::from('y');

    EXPECT_EQ(set.symbols().size(), 2U);
    EXPECT_TRUE(set.symbols().contains('x'));
    EXPECT_TRUE(set.symbols().contains('y'));
}

TEST(Set_test, Adding_a_symbol_adds_it)
{
    Set set{Set::from('x')};

    set += 'y';

    EXPECT_EQ(set.symbols().size(), 2U);
    EXPECT_TRUE(set.symbols().contains('x'));
    EXPECT_TRUE(set.symbols().contains('y'));
}

TEST(Set_test, Adding_a_list_adds_every_listed_symbol)
{
    Set set{Set::from('x')};

    set += {'y', 'z'};

    EXPECT_EQ(set.symbols().size(), 3U);
    EXPECT_TRUE(set.symbols().contains('x'));
    EXPECT_TRUE(set.symbols().contains('y'));
    EXPECT_TRUE(set.symbols().contains('z'));
}

TEST(Set_test, The_sum_of_two_sets_is_their_union)
{
    const Set left{Set::from('x')};

    const Set right{Set::from('y')};

    const Set set{left + right};

    EXPECT_EQ(set.symbols().size(), 2U);
    EXPECT_TRUE(set.symbols().contains('x'));
    EXPECT_TRUE(set.symbols().contains('y'));
}

TEST(Set_test, A_set_plus_a_symbol_holds_both)
{
    const Set set{Set::from('x') + 'y'};

    EXPECT_EQ(set.symbols().size(), 2U);
    EXPECT_TRUE(set.symbols().contains('x'));
    EXPECT_TRUE(set.symbols().contains('y'));
}

TEST(Set_test, Subtracting_a_set_removes_its_symbols)
{
    Set set{Set::from({'a', 'b', 'c', 'd'})};

    set -= Set::from({'b', 'c'});

    EXPECT_EQ(set.symbols().size(), 2U);
    EXPECT_TRUE(set.symbols().contains('a'));
    EXPECT_TRUE(set.symbols().contains('d'));
}

TEST(Set_test, Subtracting_a_symbol_removes_it)
{
    Set set{Set::from({'a', 'b', 'c'})};

    set -= 'b';

    EXPECT_EQ(set.symbols().size(), 2U);
    EXPECT_TRUE(set.symbols().contains('a'));
    EXPECT_TRUE(set.symbols().contains('c'));
}

TEST(Set_test, Subtracting_a_list_removes_every_listed_symbol)
{
    Set set{Set::from({'a', 'b', 'c', 'd'})};

    set -= {'b', 'c'};

    EXPECT_EQ(set.symbols().size(), 2U);
    EXPECT_TRUE(set.symbols().contains('a'));
    EXPECT_TRUE(set.symbols().contains('d'));
}

TEST(Set_test, The_difference_of_two_sets_keeps_the_left_symbols_the_right_lacks)
{
    const Set set{Set::from({'a', 'b', 'c', 'd'})};

    const Set difference{set - Set::from({'b', 'c'})};

    EXPECT_EQ(difference.symbols().size(), 2U);
    EXPECT_TRUE(difference.symbols().contains('a'));
    EXPECT_TRUE(difference.symbols().contains('d'));
}

TEST(Set_test, A_set_minus_a_symbol_lacks_it)
{
    const Set set{Set::from({'a', 'b', 'c'})};

    const Set difference{set - 'b'};

    EXPECT_EQ(difference.symbols().size(), 2U);
    EXPECT_TRUE(difference.symbols().contains('a'));
    EXPECT_TRUE(difference.symbols().contains('c'));
}

TEST(Set_test, A_symbol_plus_a_set_holds_both)
{
    const Set set{'x' + Set::from('y')};

    EXPECT_EQ(set.symbols().size(), 2U);
    EXPECT_TRUE(set.symbols().contains('x'));
    EXPECT_TRUE(set.symbols().contains('y'));
}

TEST(Set_test, The_union_of_overlapping_sets_holds_each_symbol_once)
{
    const Set left{Set::from({'a', 'b', 'c'})};

    const Set right{Set::from({'b', 'c', 'd'})};

    const Set set{left + right};

    EXPECT_EQ(set.symbols().size(), 4U);
    EXPECT_TRUE(set.symbols().contains('a'));
    EXPECT_TRUE(set.symbols().contains('b'));
    EXPECT_TRUE(set.symbols().contains('c'));
    EXPECT_TRUE(set.symbols().contains('d'));
}

TEST(Set_test, Subtracting_an_absent_symbol_changes_nothing)
{
    Set set{'a', 'b', 'c'};

    // 'd' is not in the set.
    set -= 'd';

    EXPECT_EQ(set.symbols().size(), 3U);
    EXPECT_TRUE(set.symbols().contains('a'));
    EXPECT_TRUE(set.symbols().contains('b'));
    EXPECT_TRUE(set.symbols().contains('c'));
}

TEST(Set_test, Subtracting_every_symbol_leaves_the_set_empty)
{
    Set set{'a', 'b', 'c'};

    set -= Set::from({'a', 'b', 'c'});

    EXPECT_TRUE(set.symbols().empty());
}

TEST(Set_test, Every_byte_less_the_printable_ones_leaves_one_hundred_sixty_one)
{
    Set set{Set::all()};

    EXPECT_EQ(set.symbols().size(), 256U);

    set -= Set::printable();

    // 256 bytes less the 95 printable ones.
    EXPECT_EQ(set.symbols().size(), 161U);
}

TEST(Set_test, A_copy_holds_the_same_symbols)
{
    const Set original{Set::from({'a', 'b', 'c'})};

    const Set copy{original};

    EXPECT_EQ(original.symbols(), copy.symbols());
}

TEST(Set_test, A_copy_assigned_holds_the_same_symbols)
{
    const Set original{Set::from({'a', 'b', 'c'})};

    Set copy{};

    copy = original;

    EXPECT_EQ(original.symbols(), copy.symbols());
}

TEST(Set_test, A_moved_set_keeps_the_symbols)
{
    Set source{'a', 'b', 'c'};

    const Set moved{std::move(source)};

    EXPECT_EQ(moved.symbols().size(), 3U);
}

TEST(Set_test, A_move_assigned_set_keeps_the_symbols)
{
    Set source{'a', 'b', 'c'};

    Set moved{};

    moved = std::move(source);

    EXPECT_EQ(moved.symbols().size(), 3U);
}

TEST(Set_test, Self_subtraction_empties_the_set)
{
    // Subtracting a set from itself must not erase the elements being iterated; the answer is defined as the empty set
    // and touches no dangling iterator.
    Set set{'a', 'b', 'c'};

    set -= set;

    EXPECT_TRUE(set.symbols().empty());
}
