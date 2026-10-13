#ifndef MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_TOKEN_AUTOMATON_HPP
#define MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_TOKEN_AUTOMATON_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "munch/verifier/merge_tower.hpp"
#include "munch/verifier/verifier_decisions.hpp"

namespace munch::verifier
{
/**
 * @brief One pair of an encoder's canonical adjacency relation: two canonical tokens, in order, into which the encoder
 *        segments their concatenation.
 */
struct Adjacent
{
    /**
     * @brief The left token, spelled by its bytes.
     */
    std::string left{};

    /**
     * @brief The right token, spelled by its bytes.
     */
    std::string right{};
};

/**
 * @brief The token automaton of canonical byte-pair encoding over a merge table, whose language is the encoder's
 *        segmentations, read token by token.
 *
 * The encoder is merge_tower()'s, the lowest-ranked merge present anywhere applied at its leftmost occurrence until
 * none applies. The automaton is the one of Berglund, Martens and van der Merwe (Constructing a BPE Tokenization DFA,
 * Theoretical Computer Science 2026, by Theorem 1, Corollary 2 and Algorithm 2 of its preprint arXiv:2405.07671v2),
 * built merge by merge by their Algorithm 2: its language is the encoder's segmentations by their Theorem 1, and a
 * transition on a token enters the same state wherever it is read, by their Corollary 2, so the tokens that may follow
 * a run depend only on the state its last token entered. A state other than the base state is stored as its parent, the
 * token numbered after its merge's product and the tokens its merge removed, and whether a token may follow a state is
 * read off the state's ancestry. token_automaton() builds it, and miscovering() decides a window certificate on it
 * without building the tower.
 *
 * token_automaton(tokens, pairs) builds the automaton from an encoder's canonical tokens and canonical pairs instead,
 * one state per token after the base state, the state of a token storing the tokens that may not follow it, so the same
 * decisions run on it for any encoder whose adjacency relation is known, with or without a merge table.
 */
class Token_automaton
{
public:
    /**
     * @brief A token of the table's vocabulary: a symbol of the alphabet by its place in it, so a byte by its value, or
     *        a product by its merge's rank past the symbols; or a canonical token of an adjacency relation by its place
     *        among the tokens given.
     */
    using Token_t = std::size_t;

    /**
     * @brief A state of the automaton, the base state 0 and then one per merge whose product some transition reads, or
     *        one per token of an adjacency relation.
     */
    using State_t = std::size_t;

    /**
     * @brief A token of the automaton: its spelling, its left part, and the state a transition on it enters.
     */
    struct Token
    {
        /**
         * @brief The bytes the token spells.
         */
        std::string spelling{};

        /**
         * @brief The left part of the token's merge, std::nullopt for a symbol and for a token of an adjacency
         *        relation.
         */
        std::optional<Token_t> left{};

        /**
         * @brief The state every transition on the token enters, std::nullopt for a product no transition reads.
         */
        std::optional<State_t> target{};
    };

    /**
     * @brief Returns the tokens, the alphabet's symbols first and then each merge's product at its rank past them, or
     *        the canonical tokens of an adjacency relation in the order given.
     * @return The tokens by number.
     */
    [[nodiscard]] const std::vector<Token>& tokens() const noexcept;

    /**
     * @brief Returns the token a spelling names.
     * @param spelling The spelling.
     * @return The token, or std::nullopt when no symbol or product is spelled so.
     */
    [[nodiscard]] std::optional<Token_t> token_of(std::string_view spelling) const;

    /**
     * @brief Returns the number of states, which are numbered densely from zero, the base state first.
     * @return The state count.
     */
    [[nodiscard]] State_t state_count() const noexcept;

    /**
     * @brief Returns whether a token may be read in a state: the token is read by some transition, and no state on the
     *        ancestry younger than the token removed it, a token younger than a state being read wherever its left part
     *        is.
     * @param state The state.
     * @param token The token.
     * @return True when a transition on the token leaves the state.
     */
    [[nodiscard]] bool is_allowed(State_t state, Token_t token) const;

    /**
     * @brief Returns the tokens that may be read in a state, each token is_allowed() admits there.
     * @param state The state.
     * @return The tokens, ascending.
     */
    [[nodiscard]] std::vector<Token_t> allowed_tokens(State_t state) const;

private:
    // token_automaton() builds the automaton over the bytes, over an alphabet of characters or from an adjacency
    // relation through the constructors.
    friend Token_automaton token_automaton(std::span<const Merge> merges);
    friend Token_automaton token_automaton(std::span<const Merge> merges, std::string_view alphabet);
    friend Token_automaton token_automaton(std::span<const std::string> tokens, std::span<const Adjacent> pairs);

    /**
     * @brief A state of the automaton other than the base state: the state it was copied from, the token numbered after
     *        its merge's product, and the tokens its merge removed from the copy; for a token of an adjacency relation,
     *        the base state, the number of tokens, and the tokens that may not follow the token.
     */
    struct State
    {
        /**
         * @brief The state it was copied from, the target of the merge's left part when the merge was applied, or the
         *        base state.
         */
        State_t parent{};

        /**
         * @brief The token numbered after the merge's product: every older token is decided at the state or above it,
         *        every younger one by its left part.
         */
        Token_t birth{};

        /**
         * @brief The tokens the state may not read where its parent may, ascending: the merge's right part, and the
         *        product too when the two parts are equal, or the tokens no pair of the relation lets follow the token.
         */
        std::vector<Token_t> removed{};
    };

    /**
     * @brief The base state, the automaton of every string over the alphabet read symbol by symbol, in which every
     *        token may be read.
     */
    static constexpr State_t base_state{0};

    /**
     * @brief Builds the automaton of a merge table over an alphabet: each symbol a token the base state reads, and then
     *        each merge in rank order, reading is_allowed() on the states built so far.
     * @param merges The merge table in rank order, each part a symbol or the product of an earlier merge.
     * @param symbols The alphabet's symbols, each spelled by its bytes, numbered in order.
     * @throws std::invalid_argument If the alphabet holds a symbol twice, or a merge names a part that neither the
     *         alphabet nor an earlier merge carries, or a product that the alphabet or an earlier merge already
     *         carries.
     */
    Token_automaton(std::span<const Merge> merges, std::span<const std::string> symbols);

    /**
     * @brief Builds the automaton of an adjacency relation: each token read by the base state and entering a state of
     *        its own, which may not read the tokens no pair lets follow it.
     * @param tokens The canonical tokens, each spelled by its bytes, numbered in order.
     * @param pairs The canonical pairs, each two of the tokens.
     * @throws std::invalid_argument If the tokens hold a spelling twice, or a pair names a spelling they do not hold.
     */
    Token_automaton(std::span<const std::string> tokens, std::span<const Adjacent> pairs);

    /**
     * @brief The tokens, the alphabet's symbols first and then each merge's product at its rank past them, or the
     *        canonical tokens of an adjacency relation.
     */
    std::vector<Token> tokens_{};

    /**
     * @brief The states, the base state first and then one per merge whose product some transition reads, or one per
     *        token of an adjacency relation.
     */
    std::vector<State> states_{};

    /**
     * @brief The token of each spelling.
     */
    std::unordered_map<std::string, Token_t> index_{};
};

/**
 * @brief Builds the token automaton of a merge table by Algorithm 2 of Berglund, Martens and van der Merwe: from the
 *        base state, each merge in rank order copies the target of its left part to a fresh state that may not read its
 *        right part, redirects the left part to the copy, and gives the product the right part's target.
 *
 * A merge whose right part cannot be read in its left part's target makes a product no transition reads, and no state.
 * The automaton has one base state plus one per merge whose product can be read, each merge costing one adjacency test,
 * and on a large table the build costs far more than a decision on it, so a caller deciding many windows builds it
 * once. The theorem miscovering() rests on places no condition on the table, but the automaton needs the products
 * distinct from the bytes and from one another, as merge_tower() does, so it takes an admissible table, as Merge
 * states, and refuses any other the same way.
 * @param merges The admissible merge table in rank order, each part a byte or the product of an earlier merge.
 * @return The token automaton.
 * @throws std::invalid_argument If a merge names a part that neither the alphabet nor an earlier merge carries, or a
 *         product that the alphabet or an earlier merge already carries; the message names the merge and its rank,
 *         each spelling quoted, a byte outside printable ASCII written `\xHH` and the quote and the backslash escaped.
 */
[[nodiscard]] Token_automaton token_automaton(std::span<const Merge> merges);

/**
 * @brief Builds the token automaton of a merge table over an alphabet of characters, each spelled by its UTF-8 bytes,
 *        by the same Algorithm 2 from a base state that reads every character of the alphabet.
 *
 * The tokens before any merge are the alphabet's characters in place of the bytes, numbered in its order, as in a
 * character-level table such as SentencePiece's, and each part of a merge is one of them or the product of an earlier
 * merge, spelled in UTF-8; the products follow at their ranks past the characters. miscovering() then decides a window
 * certificate over the strings of the alphabet by the same search, comparing spellings as bytes. No character's
 * encoding is a prefix of another's, and each begins with a byte that is not a continuation byte, so the search finds a
 * window of characters only at character boundaries, a token's spelling neither beginning nor ending inside a
 * character. The verdict is miscovering()'s on merge_tower(merges, alphabet). Over the characters U+0000 to U+007F a
 * window of them is decided as on the byte automaton of the same table, to the byte.
 * @param merges The admissible merge table in rank order, each part a character or the product of an earlier merge.
 * @param alphabet The alphabet, UTF-8 text whose characters are the tokens before any merge, each once.
 * @return The token automaton.
 * @throws std::invalid_argument If the alphabet holds a character twice, or a merge names a part that neither the
 *         alphabet nor an earlier merge carries, or a product that the alphabet or an earlier merge already carries;
 *         the message names the character, or the merge and its rank, each spelling quoted, a byte outside printable
 *         ASCII written `\xHH` and the quote and the backslash escaped.
 */
[[nodiscard]] Token_automaton token_automaton(std::span<const Merge> merges, std::string_view alphabet);

/**
 * @brief Builds the token automaton of a merge encoder from its canonical adjacency relation: the canonical tokens V,
 *        each a string the encoder segments as one token, and the canonical pairs E, each two tokens of V whose
 *        concatenation the encoder segments into the two, one state per token reading the tokens a pair lets follow it.
 *
 * The relation must be the encoder's own, which the caller vouches for, and the encoder must be projective. A merge
 * process starts from the symbols of its input and joins one adjacent pair of tokens at a time, never splitting one,
 * until no pair it may join remains; it is projective when, for every input and every interval of it, the merges of a
 * run that lie inside the interval, up to the first merge across an end of it, begin the run on the interval's string
 * alone, and make the whole of that run when no merge crosses an end. Every key rule is projective, a rule that joins,
 * among the adjacent pairs it may join, one of least key under any function of the pair, the leftmost or the rightmost
 * of them: canonical byte-pair encoding over a merge table, the lowest rank first and the leftmost, and SentencePiece's
 * own loop, the highest score first and the leftmost, with its ties between pieces of one score, such as ab and ba,
 * which no rank order of a table reproduces. So is the pass order, which applies the highest-priority merge left to
 * right until it no longer applies before choosing again, although its choice depends on the pass under way. A rule
 * that reads more of the sequence than the pair need not be: joining the pair that occurs most often, with the pieces
 * aa and ab at one score, segments aab as aa|b and aabab as a|ab|ab, so (a, ab) follows in a segmentation and is not a
 * canonical pair.
 *
 * For a projective encoder the canonical adjacency theorem holds: a list of tokens is the encoder's segmentation of its
 * spelling exactly when every token is in V and every adjacent pair in E. It is Theorem 2 of the report The Left Ripple
 * of Byte-Pair Encoding for canonical byte-pair encoding, and Theorem 1 of Appendix B of Vieira et al. (Language Models
 * over Canonical Byte-Pair Encodings, ICML 2025, arXiv:2506.07956v1) for byte-pair encoding over a merge list whose
 * every token is canonical; the report's proof uses of the encoder only that a merge never restores a boundary and its
 * projection lemma, which is the projective property, so it holds for the class unchanged. miscovering() rests on the
 * theorem alone, so on this automaton it decides a window certificate under the encoder from V and E, with no table and
 * the same bound, over the strings a chain of the tokens spells, every string over the encoder's symbols when V holds
 * them all.
 *
 * The state of a token stores the tokens that may not follow it, ascending, so the automaton holds |V| + 1 states and
 * |V|^2 - |E| stored tokens, and an adjacency test is a binary search among them.
 * @param tokens The canonical tokens V, each spelled by its bytes, numbered in order.
 * @param pairs The canonical pairs E, each two tokens of V, in any order.
 * @return The token automaton.
 * @throws std::invalid_argument If the tokens hold a spelling twice, or a pair names a spelling the tokens do not hold;
 *         the message names the spelling, or the pair and the spelling, each quoted, a byte outside printable ASCII
 *         written `\xHH` and the quote and the backslash escaped.
 */
[[nodiscard]] Token_automaton token_automaton(std::span<const std::string> tokens, std::span<const Adjacent> pairs);

/**
 * @brief Decides the window certificate (window, origin) under canonical byte-pair encoding on the token automaton of a
 *        merge table: the token containing an occurrence's final byte begins at the occurrence's byte origin, at every
 *        occurrence in every string over the automaton's alphabet under the encoder's segmentation of it.
 *
 * The verdict is miscovering()'s on the merge tower of the same table and alphabet, decided without building the tower.
 * It rests on the canonical adjacency theorem, Theorem 2 of the report The Left Ripple of Byte-Pair Encoding: a list of
 * tokens is the encoder's segmentation of its spelling exactly when every token is the segmentation of its own spelling
 * and so is every adjacent pair. A run of consecutive tokens of a segmentation is then the segmentation of its own
 * spelling, so a miscovering, when there is one, is spelled by the tokens over one occurrence of the window, from the
 * token containing its first byte to the token containing its last, and is at most 2L + |window| - 2 bytes long, L the
 * longest token. Adjacency is read off the automaton, the state a token entered deciding which tokens may follow it. On
 * the automaton of an adjacency relation the certificate is decided by the same search under the encoder whose relation
 * it is, over the strings its chains spell, since token_automaton() over a relation assumes a projective encoder, for
 * which the theorem holds.
 *
 * The search runs over nodes (state, k), the state the last token entered and the number of the window's bytes spelled
 * so far. A token holding the whole window is a miscovering by itself, unless it holds it at its start and the origin
 * is zero. A token whose spelling ends with the window's first k bytes, k short of the window's length, seeds the node
 * of its state at k. A node takes every token that may follow its state and spells the window's next bytes short of its
 * last, entering the node of the token's state further on, and, when k is not the origin, ends the search at the first
 * token that may follow its state and begins with the rest of the window, since that token contains the final byte and
 * begins at k. Nodes are interned and expanded on demand, and the search stops at the first miscovering.
 *
 * A decision reads every token's spelling against the window, O(|V| L |window|) byte comparisons for V the tokens, and
 * explores at most (|window| - 1) |Q| nodes, Q the automaton's states, each with at most |window| - 2 adjacency tests
 * for the tokens inside the window and, off the origin, one test per token beginning with the rest of the window until
 * the first that may follow. On the automaton of a merge table a test walks up the state's ancestors, taking a token's
 * left part wherever the token is younger than the state, so it costs at most the automaton's depth plus the token's
 * depth of left parts; on the automaton of an adjacency relation every token's state is a child of the base state, and
 * a test is one binary search among the at most |V| tokens it forbids, O(log |V|).
 * @param automaton The token automaton of the merge table or of an adjacency relation.
 * @param window The window, nonempty.
 * @param origin The certified origin, a byte index of the window.
 * @return std::nullopt when the certificate holds, otherwise the encoder's segmentation of a string of at most
 *         2L + |window| - 2 bytes with an occurrence whose covering boundary is not the origin, the first the search
 *         finds and not necessarily the shortest.
 * @throws std::invalid_argument If the window is empty or the origin lies outside it.
 */
[[nodiscard]] std::optional<Miscovering> miscovering(
        const Token_automaton& automaton, std::string_view window, std::size_t origin);

/**
 * @brief Decides whether the encoders of two token automata segment every string alike, and returns a string they
 *        segment apart when not: a token canonical under one and not the other, or else the spelling of a pair of
 *        tokens canonical under one that the other segments otherwise, at most 2L - 1 bytes, L the longest token.
 *
 * Two projective encoders over one alphabet, as token_automaton() over a relation states the class, segment every
 * string alike exactly when their canonical tokens V and their canonical pairs E agree: if they agree, each
 * segmentation of a string is the one chain of V and E that spells it, by the canonical adjacency theorem, so the two
 * are equal. If they differ, a shortest string they segment apart is a token of one V that the other lacks, at most L
 * bytes, or the spelling of a canonical pair of each, split at two different places, at most 2L - 1 bytes. When the V
 * agree, the first boundary of one encoder's segmentation of that string that the other's run crosses lies between two
 * of its tokens, and by projection the other segments their spelling otherwise, so that spelling is the whole string
 * and a canonical pair of the one; so it is of the other too, split elsewhere, and two splits of a string of 2L bytes
 * would both fall at L bytes. The two therefore agree on every string exactly when they agree on every string of at
 * most 2L - 1 bytes, and exactly when they agree on the |V| tokens and the |V|^2 pairs, which this decision compares.
 * It is the acceptance test of a loader that compiles a model's group of equal-scored pieces into a rank table: the
 * table is taken exactly when its automaton and the automaton of the model's own relation do not diverge, and the
 * witness is otherwise a string of at most 2L - 1 bytes on which the table segments otherwise than the model.
 *
 * A token is canonical under an automaton when a transition reads it, and a pair when the state its left token enters
 * may read its right token. The disagreement returned is the first token of the first automaton, in number order, that
 * the second does not hold canonical, else the first such token of the second, else, the canonical tokens agreeing, the
 * shortest spelling of a pair canonical under exactly one, the first of that length in the first automaton's token
 * order, which is a shortest string the two segment apart. The decision makes at most 2 |V|^2 adjacency tests.
 * @param first The first token automaton.
 * @param second The second token automaton, over the same alphabet.
 * @return std::nullopt when the two encoders segment every string alike, otherwise the witness.
 */
[[nodiscard]] std::optional<std::string> encoder_divergence(
        const Token_automaton& first, const Token_automaton& second);

} // namespace munch::verifier

#endif // MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_TOKEN_AUTOMATON_HPP
