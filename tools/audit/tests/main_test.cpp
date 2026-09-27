#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

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
    std::string out;

    /**
     * @brief What it wrote to standard error.
     */
    std::string err;
};

/**
 * @brief The directory a test's runs write their streams and files into, one per test and process.
 * @return The directory, created.
 */
std::filesystem::path scratch()
{
    const auto dir{
            std::filesystem::temp_directory_path() /
            std::format(
                    "munch-audit-{}-{}", testing::UnitTest::GetInstance()->current_test_info()->name(), ::getpid())};

    std::filesystem::create_directories(dir);

    return dir;
}

/**
 * @brief The whole of a file.
 * @param path The file's path.
 * @return Its text.
 */
std::string contents(const std::filesystem::path& path)
{
    std::ifstream stream{path, std::ios::binary};

    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/**
 * @brief The path of one of the grammars beside the tests.
 * @param name The file's name.
 * @return The path.
 */
std::string grammar(const std::string_view name)
{
    return std::string{SOURCE_DIR} + "/tools/audit/grammars/" + std::string{name};
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
            out += byte == '\'' ? std::string{R"('\'')"} : std::string{byte};
        }

        return out + '\'';
    }};

    const auto dir{scratch()};

    auto command{quoted(MUNCH_AUDIT)};

    for (const auto& argument : arguments)
    {
        command += ' ' + quoted(argument);
    }

    command += std::format(" >{} 2>{}", quoted((dir / "out").string()), quoted((dir / "err").string()));

    const auto status{std::system(command.c_str())};

    Run ran{.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1,
            .out = contents(dir / "out"),
            .err = contents(dir / "err")};

    std::filesystem::remove(dir / "out");

    std::filesystem::remove(dir / "err");

    // The directory goes with the streams unless the test keeps a file of its own in it.
    std::error_code kept;

    std::filesystem::remove(dir, kept);

    return ran;
}

/**
 * @brief The line munch-audit writes on standard error for a requirement a condition does not meet.
 * @param path The file's path as the command line named it.
 * @param line The line the scanner opens on.
 * @param text The byte as the command line spelled it.
 * @param modulo Whether the requirement was the modulo-discarded one.
 * @return The line, its newline included.
 */
std::string unmet(const std::string_view path, const int line, const std::string_view text, const bool modulo)
{
    return std::format(
            "munch-audit: {}, scanner at line {}, condition INITIAL: '{}' is not certified{}\n", path, line, text,
            modulo ? " modulo discarded" : "");
}

} // namespace

TEST(Require_certified, A_certified_byte_exits_zero_with_the_report_unchanged)
{
    const auto path{grammar("c-like-split-friendly.l")};

    const auto [status, out, err]{run({path, "--require-certified", R"(\n)"})};

    EXPECT_EQ(status, 0);
    EXPECT_EQ(err, "");
    EXPECT_EQ(out, run({path}).out);
}

TEST(Require_certified, A_lost_byte_exits_three_naming_the_condition_after_the_whole_report)
{
    const auto path{grammar("c-like-block-comments.l")};

    const auto [status, out, err]{run({path, "--require-certified", R"(\n)", "--require-certified", "0x0A"})};

    EXPECT_EQ(status, 3);
    EXPECT_EQ(err, unmet(path, 9, R"(\n)", false) + unmet(path, 9, "0x0A", false));
    EXPECT_EQ(out, run({path}).out);
}

TEST(Require_certified, The_modulo_form_asks_for_the_certified_modulo_discarded_row)
{
    const auto conventional{grammar("c-like-conventional.l")};

    EXPECT_EQ(run({conventional, "--require-certified-modulo", R"(\n)"}).status, 0);

    const auto exact{run({conventional, "--require-certified", R"(\n)"})};

    EXPECT_EQ(exact.status, 3);
    EXPECT_EQ(exact.err, unmet(conventional, 10, R"(\n)", false));

    const auto json{grammar("json.l")};

    const auto whitespace{
            run({json, "--require-certified-modulo", R"(\t)", "--require-certified-modulo", R"(\n)",
                 "--require-certified-modulo", R"(\r)"})};

    EXPECT_EQ(whitespace.status, 0);
    EXPECT_EQ(whitespace.err, "");

    const auto comments{grammar("c-like-block-comments.l")};

    const auto lost{run({comments, "--require-certified-modulo", R"(\n)"})};

    EXPECT_EQ(lost.status, 3);
    EXPECT_EQ(lost.err, unmet(comments, 9, R"(\n)", true));
}

TEST(Require_certified, A_requirement_leaves_the_json_document_alone)
{
    const auto path{grammar("json.l")};

    const auto [status, out, err]{run({path, "--json", "--require-certified", R"(\n)"})};

    EXPECT_EQ(status, 3);
    EXPECT_EQ(err, unmet(path, 11, R"(\n)", false));
    EXPECT_EQ(out, run({path, "--json"}).out);
}

TEST(Require_certified, A_malformed_byte_exits_two_with_the_usage)
{
    const auto path{grammar("json.l")};

    const auto malformed{run({path, "--require-certified", "0xZZ"})};

    EXPECT_EQ(malformed.status, 2);
    EXPECT_EQ(malformed.out, "");
    EXPECT_TRUE(malformed.err.starts_with("munch-audit: '0xZZ' is not a byte\n\nusage: munch-audit")) << malformed.err;

    const auto missing{run({path, "--require-certified-modulo"})};

    EXPECT_EQ(missing.status, 2);
    EXPECT_TRUE(missing.err.starts_with("munch-audit: --require-certified-modulo needs a value\n\nusage:"))
            << missing.err;
}

TEST(Require_certified, A_refusal_outranks_an_unmet_requirement)
{
    const auto refused{(scratch() / "empty.l").string()};

    std::ofstream{refused} << "no rules section here\n";

    const auto alone{run({refused, "--require-certified", R"(\n)"})};

    EXPECT_EQ(alone.status, 1);
    EXPECT_EQ(alone.err, "");

    const auto comments{grammar("c-like-block-comments.l")};

    const auto beside{run({comments, refused, "--require-certified", R"(\n)"})};

    EXPECT_EQ(beside.status, 1);
    EXPECT_EQ(beside.err, unmet(comments, 9, R"(\n)", false));
    EXPECT_NE(beside.out.find("refused: line 2: the file has no rules section"), std::string::npos) << beside.out;

    std::filesystem::remove_all(scratch());
}
