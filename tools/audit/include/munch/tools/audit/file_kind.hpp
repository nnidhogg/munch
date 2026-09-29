#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FILE_KIND_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FILE_KIND_HPP

#include <cstdint>
#include <string_view>

/**
 * @brief The generator a file was written for, Kind, and the kind its text says, kind_of(), which chooses the reader
 *        the command reads the file with unless the command line names one.
 */
namespace munch::tools::audit
{
/**
 * @brief The generator a file was written for, which chooses its reader.
 */
enum class Kind : std::uint8_t
{
    flex,
    re2c,
    antlr,
    logos
};

/**
 * @brief The kind a file's text says: ANTLR when its first item is a grammar declaration; logos when a derive names
 *        Logos; re2c when it opens a re2c block, which no other file does; flex otherwise. The name says nothing,
 *        since re2c lives in files of any extension and PHP's re2c scanners end in `.l`. Each question is asked of
 *        the text in the language it asks about, since the three do not lex alike: Rust nests its block comments
 *        and writes a lifetime where C writes a character literal, so a marker inside a nested comment or after a
 *        lifetime opens nothing, and an ANTLR character set holding a re2c opener is the set's bytes. A literal,
 *        comment and a raw string hold no marker in any of them.
 * @param source The file's text.
 * @return The kind.
 */
[[nodiscard]] Kind kind_of(std::string_view source) noexcept;

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FILE_KIND_HPP
