#include "munch/dfa/tools/graphviz.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

#include "munch/dfa/builder.hpp"
#include "munch/dfa/dfa.hpp"

using namespace munch::dfa;
using namespace munch::dfa::tools;

namespace
{
/**
 * @brief The DOT text of two_state_dfa().
 */
constexpr std::string_view two_state_dot{R"dot(digraph DFA {
    rankdir=LR;
    ratio=1.0;
    node [shape = circle];
    1 [shape = doublecircle, label="1 (1)"];
    __start__ [shape = none, label=""];
    __start__ -> 0;
    0 -> 1 [label = "a"];
}
)dot"};

/**
 * @brief Builds the DFA q0 -a-> q1, q1 accepting token 1.
 * @return The DFA.
 */
Dfa two_state_dfa()
{
    Builder builder{};

    const auto q0{builder.init_state()};

    const auto q1{builder.next_state()};

    const Token token{1};

    builder.add_accept_state(q1, token);

    builder.add_transition(q0, Label{'a'}, q1);

    return builder.build();
}

/**
 * @brief Expects the graph's elements besides its transitions: the layout, the node shape and the accepting q1.
 * @param dot The DOT text.
 */
void expect_graph_frame(const std::string_view dot)
{
    EXPECT_TRUE(dot.contains("rankdir=LR"));
    EXPECT_TRUE(dot.contains("node [shape = circle]"));
    EXPECT_TRUE(dot.contains(R"dot(1 [shape = doublecircle, label="1 (1)"])dot"));
}

/**
 * @brief Reads an open file to its end.
 * @param file The file.
 * @return Its contents.
 */
std::string contents_of(const std::ifstream& file)
{
    std::stringstream buffer{};

    buffer << file.rdbuf();

    return buffer.str();
}

} // namespace

TEST(Graphviz_test, To_dot_renders_the_states_the_start_and_the_transitions)
{
    const auto dfa{two_state_dfa()};

    const std::string dot_output{Graphviz::to_dot(dfa)};

    EXPECT_EQ(dot_output, two_state_dot);
}

TEST(Graphviz_test, To_file_writes_the_dot_text)
{
    const auto dfa{two_state_dfa()};

    const std::filesystem::path file_path{"./dfa_test_output.dot"};

    Graphviz::to_file(dfa, file_path);

    const std::ifstream file{file_path};

    ASSERT_TRUE(file.is_open());

    const auto written{contents_of(file)};

    std::ignore = std::filesystem::remove(file_path);

    EXPECT_EQ(written, two_state_dot);
}

TEST(Graphviz_test, To_file_throws_on_an_empty_path)
{
    const auto dfa{two_state_dfa()};

    EXPECT_THROW(Graphviz::to_file(dfa, ""), std::runtime_error);
}

TEST(Graphviz_test, To_file_accepts_a_bare_filename)
{
    // A bare filename has an empty parent path, for which no directory is created; only a stated directory is.
    const auto dfa{two_state_dfa()};

    const std::filesystem::path bare{"graphviz_bare_dfa_test.dot"};

    Graphviz::to_file(dfa, bare);

    EXPECT_TRUE(std::filesystem::exists(bare));

    std::ignore = std::filesystem::remove(bare);
}

TEST(Graphviz_test, To_file_throws_when_the_target_path_is_a_directory)
{
    const auto dfa{two_state_dfa()};

    const std::filesystem::path directory{"./graphviz_dir_target"};

    std::ignore = std::filesystem::create_directories(directory);

    EXPECT_THROW(Graphviz::to_file(dfa, directory), std::runtime_error);

    std::ignore = std::filesystem::remove(directory);
}

TEST(Graphviz_test, To_file_throws_when_writing_fails)
{
    const auto dfa{two_state_dfa()};

    // /dev/full opens successfully but fails every write with ENOSPC, exercising the "unable to write data" branch,
    // which is otherwise unreachable through ordinary filesystem failures.
    EXPECT_THROW(Graphviz::to_file(dfa, "/dev/full"), std::runtime_error);
}

TEST(Graphviz_test, To_dot_escapes_quotes_backslashes_newlines_and_tabs)
{
    Builder builder{};

    const auto q0{builder.init_state()};

    const auto q1{builder.next_state()};

    const Token token{1};

    builder.add_accept_state(q1, token);

    builder.add_transition(q0, Label{'"'}, q1);
    builder.add_transition(q0, Label{'\\'}, q1);
    builder.add_transition(q0, Label{'\n'}, q1);
    builder.add_transition(q0, Label{'\t'}, q1);

    const auto dfa{builder.build()};

    const auto dot_output{Graphviz::to_dot(dfa)};

    // Every transition is present with its escaped label.
    EXPECT_TRUE(dot_output.contains(R"(0 -> 1 [label = "\t"])"));
    EXPECT_TRUE(dot_output.contains(R"(0 -> 1 [label = "\n"])"));
    EXPECT_TRUE(dot_output.contains(R"(0 -> 1 [label = "\\"])"));
    EXPECT_TRUE(dot_output.contains(R"(0 -> 1 [label = "\""])"));

    expect_graph_frame(dot_output);
}

TEST(Graphviz_test, To_dot_writes_unprintable_bytes_as_hex_escapes)
{
    Builder builder{};

    const auto q0{builder.init_state()};

    const auto q1{builder.next_state()};

    const Token token{1};

    builder.add_accept_state(q1, token);

    // The start-of-heading control byte, the delete byte and a byte past ASCII.
    builder.add_transition(q0, Label{static_cast<char>(0x01)}, q1);
    builder.add_transition(q0, Label{static_cast<char>(0x7F)}, q1);
    builder.add_transition(q0, Label{static_cast<char>(0xFF)}, q1);

    const auto dfa{builder.build()};

    const auto dot_output{Graphviz::to_dot(dfa)};

    // Every transition is present with its label as a hex escape.
    EXPECT_TRUE(dot_output.contains(R"(0 -> 1 [label = "\x01"])"));
    EXPECT_TRUE(dot_output.contains(R"(0 -> 1 [label = "\x7F"])"));
    EXPECT_TRUE(dot_output.contains(R"(0 -> 1 [label = "\xFF"])"));

    expect_graph_frame(dot_output);
}
