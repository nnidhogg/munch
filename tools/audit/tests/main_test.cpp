#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
/**
 * @brief What one run of the command left behind.
 */
struct Run
{
    /**
     * @brief The exit status.
     */
    int status{};

    /**
     * @brief What it wrote to standard output.
     */
    std::string out{};

    /**
     * @brief What it wrote to standard error.
     */
    std::string err{};
};

/**
 * @brief Returns the directory a test's runs write their streams and files into, one per test and process.
 * @return The directory, created.
 */
std::filesystem::path scratch()
{
    const std::string_view test{testing::UnitTest::GetInstance()->current_test_info()->name()};

    const auto name{std::format("munch-audit-{}-{}", test, ::getpid())};

    const auto dir{std::filesystem::temp_directory_path() / name};

    std::filesystem::create_directories(dir);

    return dir;
}

/**
 * @brief Returns the whole of a file.
 * @param path The file's path.
 * @return Its text.
 */
std::string contents(const std::filesystem::path& path)
{
    std::ifstream stream{path, std::ios::binary};

    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

/**
 * @brief Returns the path of one of the grammars beside the tests.
 * @param name The file's name.
 * @return The path.
 */
std::string grammar(const std::string_view name)
{
    return std::format("{}/tools/audit/grammars/{}", SOURCE_DIR, name);
}

/**
 * @brief Runs munch-audit through the shell, each argument quoted so that it reaches the command as written.
 * @param arguments The arguments after the program's name.
 * @return The exit status and the two streams.
 */
Run run(const std::vector<std::string>& arguments)
{
    const auto quoted{[](const std::string_view text) {
        std::string out{'\''};

        for (const auto byte : text)
        {
            if (byte == '\'')
            {
                out += R"('\'')";

                continue;
            }

            out += byte;
        }

        return out + '\'';
    }};

    const auto dir{scratch()};

    auto command{quoted(MUNCH_AUDIT)};

    for (const auto& argument : arguments)
    {
        command += ' ' + quoted(argument);
    }

    const auto out_path{dir / "out"};

    const auto err_path{dir / "err"};

    const auto out_quoted{quoted(out_path.string())};

    const auto err_quoted{quoted(err_path.string())};

    command += std::format(" >{} 2>{}", out_quoted, err_quoted);

    const auto status{std::system(command.c_str())};

    const auto exit_status{WIFEXITED(status) ? WEXITSTATUS(status) : -1};

    Run ran{.status = exit_status, .out = contents(out_path), .err = contents(err_path)};

    std::filesystem::remove(out_path);

    std::filesystem::remove(err_path);

    // The directory goes with the streams unless the test keeps a file of its own in it.
    std::error_code kept{};

    std::filesystem::remove(dir, kept);

    return ran;
}

/**
 * @brief Returns the line munch-audit writes on standard error for a requirement a condition does not meet.
 * @param path The file's path as the command line named it.
 * @param line The line the scanner opens on.
 * @param text The byte as the command line spelled it.
 * @param modulo Whether the requirement was the modulo-discarded one.
 * @return The line, its newline included.
 */
std::string unmet(const std::string_view path, const int line, const std::string_view text, const bool modulo)
{
    const std::string_view kind{modulo ? " modulo discarded" : ""};

    return std::format(
            "munch-audit: {}, scanner at line {}, condition INITIAL: '{}' is not certified{}\n", path, line, text,
            kind);
}

} // namespace

TEST(Require_certified_test, A_certified_byte_exits_zero_with_the_report_unchanged)
{
    const auto path{grammar("c-like-split-friendly.l")};

    const auto [status, out, err]{run({path, "--require-certified", R"(\n)"})};

    const auto [plain_status, plain_out, plain_err]{run({path})};

    EXPECT_EQ(status, 0);
    EXPECT_EQ(err, "");
    EXPECT_EQ(out, plain_out);
}

TEST(Require_certified_test, A_lost_byte_exits_three_naming_the_condition_after_the_whole_report)
{
    const auto path{grammar("c-like-block-comments.l")};

    const auto [status, out, err]{run({path, "--require-certified", R"(\n)", "--require-certified", "0x0A"})};

    const auto [plain_status, plain_out, plain_err]{run({path})};

    const auto newline_unmet{unmet(path, 9, R"(\n)", false)};

    const auto hex_unmet{unmet(path, 9, "0x0A", false)};

    EXPECT_EQ(status, 3);
    EXPECT_EQ(err, newline_unmet + hex_unmet);
    EXPECT_EQ(out, plain_out);
}

TEST(Require_certified_test, The_modulo_form_asks_for_the_certified_modulo_discarded_row)
{
    const auto conventional{grammar("c-like-conventional.l")};

    const auto [modulo_status, modulo_out, modulo_err]{run({conventional, "--require-certified-modulo", R"(\n)"})};

    EXPECT_EQ(modulo_status, 0);

    const auto [exact_status, exact_out, exact_err]{run({conventional, "--require-certified", R"(\n)"})};

    EXPECT_EQ(exact_status, 3);
    EXPECT_EQ(exact_err, unmet(conventional, 10, R"(\n)", false));

    const auto json{grammar("json.l")};

    const auto [whitespace_status, whitespace_out, whitespace_err]{
            run({json, "--require-certified-modulo", R"(\t)", "--require-certified-modulo", R"(\n)",
                 "--require-certified-modulo", R"(\r)"})};

    EXPECT_EQ(whitespace_status, 0);
    EXPECT_EQ(whitespace_err, "");

    const auto comments{grammar("c-like-block-comments.l")};

    const auto [lost_status, lost_out, lost_err]{run({comments, "--require-certified-modulo", R"(\n)"})};

    EXPECT_EQ(lost_status, 3);
    EXPECT_EQ(lost_err, unmet(comments, 9, R"(\n)", true));
}

TEST(Require_certified_test, A_requirement_leaves_the_json_document_alone)
{
    const auto path{grammar("json.l")};

    const auto [status, out, err]{run({path, "--json", "--require-certified", R"(\n)"})};

    const auto [document_status, document_out, document_err]{run({path, "--json"})};

    EXPECT_EQ(status, 3);
    EXPECT_EQ(err, unmet(path, 11, R"(\n)", false));
    EXPECT_EQ(out, document_out);
}

TEST(Require_certified_test, A_malformed_byte_exits_two_with_the_usage)
{
    const auto path{grammar("json.l")};

    const auto [malformed_status, malformed_out, malformed_err]{run({path, "--require-certified", "0xZZ"})};

    EXPECT_EQ(malformed_status, 2);
    EXPECT_EQ(malformed_out, "");
    EXPECT_TRUE(malformed_err.starts_with("munch-audit: '0xZZ' is not a byte\n\nusage: munch-audit")) << malformed_err;

    const auto [missing_status, missing_out, missing_err]{run({path, "--require-certified-modulo"})};

    EXPECT_EQ(missing_status, 2);
    EXPECT_TRUE(missing_err.starts_with("munch-audit: --require-certified-modulo needs a value\n\nusage:"))
            << missing_err;
}

TEST(Require_certified_test, A_refusal_outranks_an_unmet_requirement)
{
    const auto directory{scratch()};

    const auto refused{(directory / "empty.l").string()};

    std::ofstream{refused} << "no rules section here\n";

    const auto [alone_status, alone_out, alone_err]{run({refused, "--require-certified", R"(\n)"})};

    EXPECT_EQ(alone_status, 1);
    EXPECT_EQ(alone_err, "");

    const auto comments{grammar("c-like-block-comments.l")};

    const auto [beside_status, beside_out, beside_err]{run({comments, refused, "--require-certified", R"(\n)"})};

    EXPECT_EQ(beside_status, 1);
    EXPECT_EQ(beside_err, unmet(comments, 9, R"(\n)", false));
    EXPECT_TRUE(beside_out.contains("refused: line 2: the file has no rules section")) << beside_out;

    std::filesystem::remove_all(directory);
}

TEST(Label_test, A_long_pattern_of_continuation_bytes_is_cut_at_its_start)
{
    // A label holds at most 60 bytes, and a pattern or token longer than that is named by its pattern cut to fit with
    // the ellipsis; no byte among the first 58 of the pattern begins a code point, so the one cut that keeps whole
    // characters stands before the first, and the label is the ellipsis alone.
    const auto directory{scratch()};

    const auto path{(directory / "continuation.l").string()};

    constexpr std::size_t label_limit{60};

    const auto pattern{std::string(label_limit + 1, '\x80')};

    constexpr std::size_t longest_token{40};

    const auto token{std::string(longest_token + 1, 'T')};

    std::ofstream{path, std::ios::binary}
            << std::format("%%\n{} {{ return {}; }}\n\"a\" {{ return 2; }}\n", pattern, token);

    const auto [status, out, err]{run({path, "--json"})};

    const std::string_view named{R"("token": {"id": 0, "name": ")"};

    const auto at{out.find(named)};

    ASSERT_NE(at, std::string::npos) << out;

    const auto begin{at + named.size()};

    const auto end{out.find('"', begin)};

    const auto label{out.substr(begin, end - begin)};

    EXPECT_EQ(status, 0);
    EXPECT_LE(label.size(), label_limit);
    EXPECT_EQ(label, "...");

    std::filesystem::remove_all(directory);
}

TEST(Price_test, A_byte_in_hex_takes_hex_digits_alone)
{
    const auto path{grammar("json.l")};

    const auto [control_status, control_out, control_err]{run({path, "--price", "0x\x11\x12"})};

    EXPECT_EQ(control_status, 2);
    EXPECT_EQ(control_out, "");
    EXPECT_TRUE(control_err.starts_with("munch-audit: '0x\x11\x12' is not a byte\n\nusage: munch-audit"))
            << control_err;

    const auto [digits_status, digits_out, digits_err]{run({path, "--price", "0x12"})};

    EXPECT_EQ(digits_status, 0);
    EXPECT_TRUE(digits_out.contains("what it would cost to certify 0x12\n")) << digits_out;

    const auto [letters_status, letters_out, letters_err]{run({path, "--price", "0xab"})};

    EXPECT_EQ(letters_status, 0);
    EXPECT_TRUE(letters_out.contains("what it would cost to certify 0xAB\n")) << letters_out;

    const auto [upper_status, upper_out, upper_err]{run({path, "--price", "0xAB"})};

    EXPECT_EQ(upper_out, letters_out);

    const auto [required_status, required_out, required_err]{run({path, "--require-certified", "0x\x11\x12"})};

    EXPECT_EQ(required_status, 2);
}

TEST(Repairs_test, Each_control_byte_json_keeps_out_of_strings_certifies_once_given_a_token_of_its_own)
{
    const auto path{grammar("json.l")};

    const auto [status, out, err]{run({path, "--repairs"})};

    const auto [plain_status, plain_out, plain_err]{run({path})};

    // The 29 control bytes but the tab, the newline and the carriage return, which no token of RFC 8259 admits.
    std::string visible{};

    std::string discarded{};

    for (unsigned value{0}; value < 0x20; ++value)
    {
        if (value == '\t' || value == '\n' || value == '\r')
        {
            continue;
        }

        const auto visible_label{std::format("visible 0x{:02X}", value)};

        const auto discarded_label{std::format("discarded 0x{:02X}", value)};

        visible += std::format("  {:<26} certifies exactly\n", visible_label);

        discarded += std::format("  {:<26} certifies once discarded tokens are deleted\n", discarded_label);
    }

    const auto section{std::format(
            "\nwhat a token of one byte's own would certify, added at the lowest priority\n{}{}", visible, discarded)};

    // The section stands after the report and before the blank line that closes the condition.
    const auto report_end{plain_out.rfind("\n\n")};

    const auto report{plain_out.substr(0, report_end + 1)};

    const auto expected{std::format("{}{}\n", report, section)};

    EXPECT_EQ(status, 0);
    EXPECT_EQ(out, expected);
}

TEST(Repairs_test, A_grammar_whose_comment_admits_every_byte_is_told_no_byte_does)
{
    const auto [status, out, err]{run({grammar("c-like-block-comments.l"), "--repairs"})};

    EXPECT_EQ(status, 0);
    EXPECT_TRUE(
            out.contains("\nwhat a token of one byte's own would certify, added at the lowest priority\n"
                         "  no byte                    certifies once given a token of its own, visible or "
                         "discarded\n"))
            << out;
}

TEST(Repairs_test, A_grammar_that_certifies_a_byte_is_left_as_it_is)
{
    const auto path{grammar("c-like-split-friendly.l")};

    const auto [text_status, text_out, text_err]{run({path, "--repairs"})};

    const auto [plain_status, plain_out, plain_err]{run({path})};

    EXPECT_EQ(text_out, plain_out);

    const auto [json_status, json_out, json_err]{run({path, "--json", "--repairs"})};

    const auto [document_status, document_out, document_err]{run({path, "--json"})};

    EXPECT_EQ(json_out, document_out);
}

TEST(Repairs_test, The_json_document_holds_the_repairs_beside_the_report)
{
    const auto path{grammar("json.l")};

    const auto [status, out, err]{run({path, "--json", "--repairs"})};

    EXPECT_EQ(status, 0);
    EXPECT_TRUE(out.contains(R"(, "repairs": {"visible": [{"byte": 0, "gained": []}, {"byte": 1, "gained": []}, )"))
            << out;
    EXPECT_TRUE(out.contains(R"({"byte": 31, "gained": []}], "discarded": [{"byte": 0, "gained": []}, )")) << out;

    const auto [comments_status, comments_out, comments_err]{
            run({grammar("c-like-block-comments.l"), "--json", "--repairs"})};

    EXPECT_TRUE(comments_out.contains(R"(, "repairs": {"visible": [], "discarded": []})")) << comments_out;
}
