#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

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
    std::string_view extension;

    /**
     * @brief The reader.
     */
    std::function<std::vector<Lexer_spec>(std::string_view)> read;

    /**
     * @brief Whether the reader reads characters rather than bytes, so that its negated sets admit the UTF-8
     *        encodings alone and the bytes no encoding uses are outside its comparison with the flex file.
     */
    bool characters;
};

/**
 * @brief The text of one of the grammars beside the tests.
 * @param name The file's name.
 * @return Its text.
 */
std::string grammar(const std::string_view name)
{
    std::ifstream stream{std::string{SOURCE_DIR} + "/tools/audit/grammars/" + std::string{name}};

    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

} // namespace

TEST(Twins, Every_grammar_cuts_alike_through_every_reader)
{
    const std::vector<Twin_reader> readers{
            {.extension = ".re",
             .read = [](const std::string_view source) { return read_re2c(source); },
             .characters = false},
            {.extension = ".g4", .read = read_antlr, .characters = true},
            {.extension = ".rs", .read = read_logos, .characters = true}};

    for (const std::string_view name :
         {"c-like-conventional", "c-like-split-friendly", "c-like-block-comments", "json", "log-lines"})
    {
        const auto from_flex{build(read_flex(grammar(std::string{name} + ".l")).front(), "INITIAL")};

        for (const auto& [extension, read, characters] : readers)
        {
            const auto scanners{read(grammar(std::string{name} + std::string{extension}))};

            ASSERT_EQ(scanners.size(), 1u) << name << extension;

            const auto twin{build(scanners.front(), "INITIAL")};

            // No input the two tokenize is cut differently, over every input rather than a sample; a character
            // reader's twin parts from the flex file only on input it refuses.
            const auto difference{from_flex.boundary_difference(twin)};

            EXPECT_TRUE(difference.exhaustive) << name << extension;
            EXPECT_TRUE(difference.witness.empty()) << name << extension << ": " << difference.witness;

            // The certificates agree on every byte, or for a character reader on every byte an encoding uses: the
            // flex file, written over bytes, consumes the others where such a reader never meets them.
            for (int value{0}; value < 256; ++value)
            {
                if (characters && (value == 0xC0 || value == 0xC1 || value >= 0xF5))
                {
                    continue;
                }

                const auto byte{static_cast<char>(value)};

                EXPECT_EQ(from_flex.is_split_point(byte), twin.is_split_point(byte))
                        << name << extension << ' ' << value;
                EXPECT_EQ(from_flex.is_split_point_ignoring(byte), twin.is_split_point_ignoring(byte))
                        << name << extension << ' ' << value;
            }
        }
    }
}
