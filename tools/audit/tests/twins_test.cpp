#include <gtest/gtest.h>

#include <cstddef>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"
#include "munch/tools/audit/read_antlr.hpp"
#include "munch/tools/audit/read_flex.hpp"
#include "munch/tools/audit/read_logos.hpp"
#include "munch/tools/audit/read_re2c.hpp"

using namespace munch::tools::audit;

namespace
{
/**
 * @brief One reader of a grammar's twin, by the extension of the file it reads.
 */
struct Twin_reader
{
    /**
     * @brief The twin file's extension.
     */
    std::string_view extension{};

    /**
     * @brief The reader, from the twin file's text to its scanners.
     */
    std::function<std::vector<Lexer_spec>(std::string_view)> read{};

    /**
     * @brief Whether the reader reads characters rather than bytes, so that its negated sets admit the UTF-8 encodings
     *        alone and the bytes no encoding uses are outside its comparison with the flex file.
     */
    bool characters{};
};

/**
 * @brief Returns the text of one of the grammars beside the tests.
 * @param name The file's name.
 * @return Its text.
 */
std::string grammar(const std::string_view name)
{
    const auto path{std::format("{}/tools/audit/grammars/{}", SOURCE_DIR, name)};

    std::ifstream stream{path};

    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

/**
 * @brief Returns whether a byte stands in no UTF-8 encoding: the overlong leads C0 and C1, and F5 on, which would lead
 *        past U+10FFFF.
 * @param value The byte's value.
 * @return True when no encoding uses it.
 */
[[nodiscard]] constexpr bool never_in_utf8(const std::size_t value) noexcept
{
    return value == 0xC0U || value == 0xC1U || value >= 0xF5U;
}

} // namespace

TEST(Twins_test, Every_grammar_cuts_alike_through_every_reader)
{
    // A character reader never meets the bytes no encoding uses, which the flex file, written over bytes, consumes.
    const auto expect_same_certificates{[](const munch::core::Lexer& from_flex, const munch::core::Lexer& twin,
                                           const bool characters, const std::string_view name,
                                           const std::string_view extension) {
        for (std::size_t value{0}; value < byte_values; ++value)
        {
            if (characters && never_in_utf8(value))
            {
                continue;
            }

            const auto byte{static_cast<char>(value)};

            EXPECT_EQ(from_flex.is_split_point(byte), twin.is_split_point(byte)) << name << extension << ' ' << value;
            EXPECT_EQ(from_flex.is_split_point_ignoring(byte), twin.is_split_point_ignoring(byte))
                    << name << extension << ' ' << value;
        }
    }};

    const std::vector<Twin_reader> readers{
            {.extension = ".re",
             .read = [](const std::string_view source) { return read_re2c(source); },
             .characters = false},
            {.extension = ".g4", .read = read_antlr, .characters = true},
            {.extension = ".rs", .read = read_logos, .characters = true}};

    for (const std::string_view name :
         {"c-like-conventional", "c-like-split-friendly", "c-like-block-comments", "json", "log-lines"})
    {
        const auto flex_file{std::format("{}.l", name)};

        const auto flex_source{grammar(flex_file)};

        const auto flex_scanners{read_flex(flex_source)};

        const auto from_flex{build(flex_scanners.front(), "INITIAL")};

        for (const auto& [extension, read, characters] : readers)
        {
            const auto twin_file{std::format("{}{}", name, extension)};

            const auto source{grammar(twin_file)};

            const auto scanners{read(source)};

            ASSERT_EQ(scanners.size(), 1U) << name << extension;

            const auto twin{build(scanners.front(), "INITIAL")};

            // No input the two tokenize is cut differently, over every input rather than a sample; a character reader's
            // twin parts from the flex file only on input it refuses.
            const auto [witness, exhaustive]{from_flex.boundary_difference(twin)};

            EXPECT_TRUE(exhaustive) << name << extension;
            EXPECT_TRUE(witness.empty()) << name << extension << ": " << witness;

            expect_same_certificates(from_flex, twin, characters, name, extension);
        }
    }
}
