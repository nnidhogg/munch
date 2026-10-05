#include "munch/nfa/tools/graphviz.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

#include "munch/nfa/builder.hpp"
#include "munch/nfa/nfa.hpp"

using namespace munch::nfa;
using namespace munch::nfa::tools;

namespace
{
/**
 * @brief The DOT text of a_to_accept().
 */
constexpr std::string_view expected_dot{R"dot(digraph NFA {
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
 * @brief Builds the NFA with one transition from q0 to q1 on each given label, q1 accepting token 1.
 * @param labels The labels.
 * @return The NFA.
 */
Nfa parallel_edges(const std::initializer_list<Label> labels)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    const Token token{1, 1};

    builder.add_accept_state(q1, token);

    for (const auto& label : labels)
    {
        builder.add_transition(q0, label, q1);
    }

    return builder.build();
}

/**
 * @brief Builds the NFA q0 -a-> q1, q1 accepting token 1.
 * @return The NFA.
 */
Nfa a_to_accept()
{
    return parallel_edges({Label{'a'}});
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
    const auto nfa{a_to_accept()};

    const auto dot_output{Graphviz::to_dot(nfa)};

    EXPECT_EQ(dot_output, expected_dot);
}

TEST(Graphviz_test, To_dot_renders_an_accept_state_without_a_token_as_n_a)
{
    // add_accept_state(state) with no token marks an intermediate accept state, as inside the constructions of kleene()
    // and optional(), rendered as "n/a" rather than a token id.
    Builder builder{};

    const auto q0{builder.init_state()};

    builder.add_accept_state(q0);

    const auto nfa{builder.build()};

    const auto dot_output{Graphviz::to_dot(nfa)};

    EXPECT_TRUE(dot_output.contains(R"dot(0 [shape = doublecircle, label="0 (n/a)"])dot"));
}

TEST(Graphviz_test, To_file_writes_the_dot_text)
{
    const auto nfa{a_to_accept()};

    const std::filesystem::path file_path{"./nfa_test_output.dot"};

    Graphviz::to_file(nfa, file_path);

    const std::ifstream file{file_path};

    ASSERT_TRUE(file.is_open());

    const auto written{contents_of(file)};

    std::ignore = std::filesystem::remove(file_path);

    EXPECT_EQ(written, expected_dot);
}

TEST(Graphviz_test, To_file_throws_on_an_empty_path)
{
    const auto nfa{a_to_accept()};

    EXPECT_THROW(Graphviz::to_file(nfa, ""), std::runtime_error);
}

TEST(Graphviz_test, To_file_accepts_a_bare_filename)
{
    // A bare filename has an empty parent path, for which no directory is created; only a stated directory is.
    const auto nfa{a_to_accept()};

    const std::filesystem::path bare{"graphviz_bare_nfa_test.dot"};

    Graphviz::to_file(nfa, bare);

    EXPECT_TRUE(std::filesystem::exists(bare));

    std::ignore = std::filesystem::remove(bare);
}

TEST(Graphviz_test, To_file_throws_when_the_target_path_is_a_directory)
{
    const auto nfa{a_to_accept()};

    const std::filesystem::path directory{"./graphviz_dir_target"};

    std::ignore = std::filesystem::create_directories(directory);

    EXPECT_THROW(Graphviz::to_file(nfa, directory), std::runtime_error);

    std::ignore = std::filesystem::remove(directory);
}

TEST(Graphviz_test, To_file_throws_when_writing_fails)
{
    const auto nfa{a_to_accept()};

    // /dev/full opens successfully but fails every write with ENOSPC, exercising the "unable to write data" branch,
    // which is otherwise unreachable through ordinary filesystem failures.
    EXPECT_THROW(Graphviz::to_file(nfa, "/dev/full"), std::runtime_error);
}

TEST(Graphviz_test, To_dot_labels_an_epsilon_transition_with_epsilon)
{
    const auto nfa{parallel_edges({Label::epsilon()})};

    const auto dot_output{Graphviz::to_dot(nfa)};

    EXPECT_TRUE(dot_output.contains(R"(0 -> 1 [label = "ε"])"));
}

TEST(Graphviz_test, To_dot_escapes_quotes_backslashes_newlines_and_tabs)
{
    const auto nfa{parallel_edges({Label{'"'}, Label{'\\'}, Label{'\n'}, Label{'\t'}})};

    const auto dot_output{Graphviz::to_dot(nfa)};

    // Every transition is present with its escaped label.
    EXPECT_TRUE(dot_output.contains(R"(0 -> 1 [label = "\t"])"));
    EXPECT_TRUE(dot_output.contains(R"(0 -> 1 [label = "\n"])"));
    EXPECT_TRUE(dot_output.contains(R"(0 -> 1 [label = "\\"])"));
    EXPECT_TRUE(dot_output.contains(R"(0 -> 1 [label = "\""])"));

    expect_graph_frame(dot_output);
}

TEST(Graphviz_test, To_dot_writes_unprintable_bytes_as_hex_escapes)
{
    // The start-of-heading control byte, the delete byte and a byte past ASCII.
    const auto nfa{parallel_edges(
            {Label{static_cast<char>(0x01)}, Label{static_cast<char>(0x7F)}, Label{static_cast<char>(0xFF)}})};

    const auto dot_output{Graphviz::to_dot(nfa)};

    // Every transition is present with its label as a hex escape.
    EXPECT_TRUE(dot_output.contains(R"(0 -> 1 [label = "\x01"])"));
    EXPECT_TRUE(dot_output.contains(R"(0 -> 1 [label = "\x7F"])"));
    EXPECT_TRUE(dot_output.contains(R"(0 -> 1 [label = "\xFF"])"));

    expect_graph_frame(dot_output);
}
