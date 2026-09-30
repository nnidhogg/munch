#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_CROSSCHECK_SESSION_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_CROSSCHECK_SESSION_HPP

#include <istream>
#include <optional>
#include <ostream>
#include <string_view>

#include "munch/core/lexer.hpp"

/**
 * @brief The cross-check probes' line protocol over one lexer at a time, Crosscheck_session.
 */
namespace munch::tools::probes
{
/**
 * @brief One lexer at a time, re-seated by each SET, and the protocol's answer to each query written as one line. A
 *        query reads its fields from the input after the command word; a string field of `-` denotes the empty string,
 *        and an empty answer string is written as `-`.
 */
class Crosscheck_session
{
public:
    /**
     * @brief The command sets a probe's protocol accepts.
     */
    enum class Protocol
    {
        /**
         * @brief SET, Q, T and END.
         */
        scan,

        /**
         * @brief SET, Q, N, M, T and END.
         */
        anchor
    };

    /**
     * @brief Reads the protocol's commands from an input until END or the input ends and answers each, refusing a query
     *        before any SET and an unknown command with `<program>: <command> before any SET` or
     *        `<program>: unknown command <command>` on the error stream.
     * @param in The input the commands are read from.
     * @param out The output the answers are written to, flushed once the commands end.
     * @param err The output a refusal is written to.
     * @param program The probe's name, which opens a refusal.
     * @param protocol The commands the probe accepts.
     * @return True when every command was answered, false after a refusal.
     */
    [[nodiscard]] bool run(
            std::istream& in, std::ostream& out, std::ostream& err, std::string_view program, Protocol protocol);

private:
    /**
     * @brief Returns whether a SET has built a lexer.
     * @return True after the first SET.
     */
    [[nodiscard]] bool ready() const noexcept;

    /**
     * @brief Answers SET: reads a count and that many literal tokens, and builds the session's lexer over them, each
     *        token's kind and priority its position in the list.
     * @param in The input the fields are read from.
     */
    void set(std::istream& in);

    /**
     * @brief Answers Q: reads an input and an offset, and writes the certificate walk's answer from that offset,
     *        `A <start> <evidence_begin> <evidence_end> <window>` or `R` when it refuses; a lexer must be set.
     * @param in The input the fields are read from.
     * @param out The output the answer is written to.
     */
    void query(std::istream& in, std::ostream& out) const;

    /**
     * @brief Answers N: reads a tail and an offset, and writes the anchored decider's answer from that offset,
     *        `N <position>` or `NR` when it refuses; a lexer must be set.
     * @param in The input the fields are read from.
     * @param out The output the answer is written to.
     */
    void anchored(std::istream& in, std::ostream& out) const;

    /**
     * @brief Answers M: reads a tail, and writes a minimal repair of it, `M <repair>` or `MR` when none exists or the
     *        token set is nullable; a lexer must be set.
     * @param in The input the field is read from.
     * @param out The output the answer is written to.
     */
    void repair(std::istream& in, std::ostream& out) const;

    /**
     * @brief Answers T: reads an input, and writes what the maximal-munch scan commits on it,
     *        `T <consumed> <token_count> <length> ...`; a lexer must be set.
     * @param in The input the field is read from.
     * @param out The output the answer is written to.
     */
    void tokenize(std::istream& in, std::ostream& out) const;

    /**
     * @brief The lexer the last SET built, std::nullopt before the first.
     */
    std::optional<core::Lexer> lexer_{};
};

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_CROSSCHECK_SESSION_HPP
