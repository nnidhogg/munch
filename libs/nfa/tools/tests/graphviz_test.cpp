#include "lexer/nfa/tools/graphviz.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "lexer/nfa/builder.hpp"
#include "lexer/nfa/nfa.hpp"

using namespace lexer;
using namespace lexer::nfa;
using namespace lexer::nfa::tools;

using Graphviz_test = testing::Test;

TEST_F(Graphviz_test, Graphviz_to_dot)
{
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q1, token);
    nfa.add_transition(q0, nfa::Label('a'), q1);

    const auto result{nfa.build()};

    const std::string dot_output = Graphviz::to_dot(result);

    const std::string expected_output =
            "digraph NFA {\n"
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
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q1, token);
    nfa.add_transition(q0, nfa::Label('a'), q1);

    const auto result{nfa.build()};

    const std::filesystem::path file_path{"./nfa_test_output.dot"};
    Graphviz::to_file(result, file_path);

    std::ifstream file(file_path);
    ASSERT_TRUE(file.is_open());

    std::stringstream buffer;
    buffer << file.rdbuf();

    const std::string expected_output{
            "digraph NFA {\n"
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
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q1, token);
    nfa.add_transition(q0, nfa::Label('a'), q1);

    const auto result{nfa.build()};

    // Test invalid file path
    EXPECT_THROW(Graphviz::to_file(result, ""), std::runtime_error);

    // Test valid file path
    const std::filesystem::path file_path{"./nfa_test_output.dot"};
    EXPECT_NO_THROW(Graphviz::to_file(result, file_path));

    std::ifstream file(file_path);
    ASSERT_TRUE(file.is_open());

    std::stringstream buffer;
    buffer << file.rdbuf();

    const std::string expected_output{
            "digraph NFA {\n"
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

TEST_F(Graphviz_test, Graphviz_to_dot_special_characters)
{
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q1, token);

    nfa.add_transition(q0, nfa::Label('"'), q1);
    nfa.add_transition(q0, nfa::Label('\\'), q1);
    nfa.add_transition(q0, nfa::Label('\n'), q1);
    nfa.add_transition(q0, nfa::Label('\t'), q1);

    const auto result{nfa.build()};

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
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q1, token);

    // Add transitions with non-printable characters
    nfa.add_transition(q0, nfa::Label(static_cast<char>(0x01)), q1); // SOH
    nfa.add_transition(q0, nfa::Label(static_cast<char>(0x7F)), q1); // DEL
    nfa.add_transition(q0, nfa::Label(static_cast<char>(0xFF)), q1); // Extended ASCII

    const auto result{nfa.build()};

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
