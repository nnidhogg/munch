#include "munch/dfa/tools/graphviz.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "munch/dfa/builder.hpp"
#include "munch/dfa/dfa.hpp"

using namespace munch;
using namespace munch::dfa;
using namespace munch::dfa::tools;

using Graphviz_test = testing::Test;

TEST_F(Graphviz_test, Graphviz_to_dot)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};

    const Token token{1};

    dfa.add_accept_state(q1, token);
    dfa.add_transition(q0, dfa::Label('a'), q1);

    const auto result{dfa.build()};

    const std::string dot_output = Graphviz::to_dot(result);

    const std::string expected_output =
            "digraph DFA {\n"
            "    rankdir=LR;\n"
            "    ratio=1.0;\n"
            "    node [shape = circle];\n"
            "    1 [shape = doublecircle, label=\"1 (1)\"];\n"
            "    __start__ [shape = none, label=\"\"];\n"
            "    __start__ -> 0;\n"
            "    0 -> 1 [label = \"a\"];\n"
            "}\n";

    EXPECT_EQ(dot_output, expected_output);
}

TEST_F(Graphviz_test, Graphviz_to_file)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};

    const Token token{1};

    dfa.add_accept_state(q1, token);
    dfa.add_transition(q0, dfa::Label('a'), q1);

    const auto result{dfa.build()};

    const std::filesystem::path file_path{"./dfa_test_output.dot"};
    Graphviz::to_file(result, file_path);

    std::ifstream file(file_path);
    ASSERT_TRUE(file.is_open());

    std::stringstream buffer;
    buffer << file.rdbuf();

    const std::string expected_output{
            "digraph DFA {\n"
            "    rankdir=LR;\n"
            "    ratio=1.0;\n"
            "    node [shape = circle];\n"
            "    1 [shape = doublecircle, label=\"1 (1)\"];\n"
            "    __start__ [shape = none, label=\"\"];\n"
            "    __start__ -> 0;\n"
            "    0 -> 1 [label = \"a\"];\n"
            "}\n"};

    EXPECT_EQ(buffer.str(), expected_output);
}

TEST_F(Graphviz_test, Graphviz_to_file_exceptions)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};

    const Token token{1};

    dfa.add_accept_state(q1, token);
    dfa.add_transition(q0, dfa::Label('a'), q1);

    const auto result{dfa.build()};

    // Test invalid file path
    EXPECT_THROW(Graphviz::to_file(result, ""), std::runtime_error);

    // Test valid file path
    const std::filesystem::path file_path{"./dfa_test_output.dot"};
    EXPECT_NO_THROW(Graphviz::to_file(result, file_path));

    std::ifstream file(file_path);
    ASSERT_TRUE(file.is_open());

    std::stringstream buffer;
    buffer << file.rdbuf();

    const std::string expected_output{
            "digraph DFA {\n"
            "    rankdir=LR;\n"
            "    ratio=1.0;\n"
            "    node [shape = circle];\n"
            "    1 [shape = doublecircle, label=\"1 (1)\"];\n"
            "    __start__ [shape = none, label=\"\"];\n"
            "    __start__ -> 0;\n"
            "    0 -> 1 [label = \"a\"];\n"
            "}\n"};

    EXPECT_EQ(buffer.str(), expected_output);
}

TEST_F(Graphviz_test, Graphviz_to_file_throws_when_the_target_path_is_a_directory)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};

    const Token token{1};

    dfa.add_accept_state(q1, token);
    dfa.add_transition(q0, dfa::Label('a'), q1);

    const auto result{dfa.build()};

    std::filesystem::create_directories("./graphviz_dir_target");
    EXPECT_THROW(Graphviz::to_file(result, "./graphviz_dir_target"), std::runtime_error);
}

TEST_F(Graphviz_test, Graphviz_to_file_throws_when_writing_fails)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};

    const Token token{1};

    dfa.add_accept_state(q1, token);
    dfa.add_transition(q0, dfa::Label('a'), q1);

    const auto result{dfa.build()};

    // /dev/full opens successfully but fails every write with ENOSPC, exercising the "unable to write data"
    // branch, which is otherwise unreachable through ordinary filesystem failures.
    EXPECT_THROW(Graphviz::to_file(result, "/dev/full"), std::runtime_error);
}

TEST_F(Graphviz_test, Graphviz_to_dot_special_characters)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};

    const Token token{1};

    dfa.add_accept_state(q1, token);

    dfa.add_transition(q0, dfa::Label('"'), q1);
    dfa.add_transition(q0, dfa::Label('\\'), q1);
    dfa.add_transition(q0, dfa::Label('\n'), q1);
    dfa.add_transition(q0, dfa::Label('\t'), q1);

    const auto result{dfa.build()};

    const auto dot_output{Graphviz::to_dot(result)};

    // Check that all expected transitions are present in the output
    EXPECT_NE(dot_output.find("0 -> 1 [label = \"\\t\"]"), std::string::npos);
    EXPECT_NE(dot_output.find("0 -> 1 [label = \"\\n\"]"), std::string::npos);
    EXPECT_NE(dot_output.find("0 -> 1 [label = \"\\\\\"]"), std::string::npos);
    EXPECT_NE(dot_output.find("0 -> 1 [label = \"\\\"\"]"), std::string::npos);

    // Check other structural elements of the dot output
    EXPECT_NE(dot_output.find("rankdir=LR"), std::string::npos);
    EXPECT_NE(dot_output.find("node [shape = circle]"), std::string::npos);
    EXPECT_NE(dot_output.find("1 [shape = doublecircle, label=\"1 (1)\"]"), std::string::npos);
}

TEST_F(Graphviz_test, Graphviz_to_dot_non_printable_characters)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};

    const Token token{1};

    dfa.add_accept_state(q1, token);

    // Add transitions with non-printable characters
    dfa.add_transition(q0, dfa::Label(static_cast<char>(0x01)), q1); // SOH
    dfa.add_transition(q0, dfa::Label(static_cast<char>(0x7F)), q1); // DEL
    dfa.add_transition(q0, dfa::Label(static_cast<char>(0xFF)), q1); // Extended ASCII

    const auto result{dfa.build()};

    const auto dot_output{Graphviz::to_dot(result)};

    // Check that all expected transitions are present in the output
    EXPECT_NE(dot_output.find("0 -> 1 [label = \"\\x01\"]"), std::string::npos);
    EXPECT_NE(dot_output.find("0 -> 1 [label = \"\\x7F\"]"), std::string::npos);
    EXPECT_NE(dot_output.find("0 -> 1 [label = \"\\xFF\"]"), std::string::npos);

    // Check other structural elements of the dot output
    EXPECT_NE(dot_output.find("rankdir=LR"), std::string::npos);
    EXPECT_NE(dot_output.find("node [shape = circle]"), std::string::npos);
    EXPECT_NE(dot_output.find("1 [shape = doublecircle, label=\"1 (1)\"]"), std::string::npos);
}
