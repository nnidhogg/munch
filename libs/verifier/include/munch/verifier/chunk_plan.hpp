#ifndef MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_CHUNK_PLAN_HPP
#define MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_CHUNK_PLAN_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "munch/verifier/piece_set.hpp"
#include "munch/verifier/token_automaton.hpp"
#include "munch/verifier/verifier.hpp"

namespace munch::verifier
{
/**
 * @brief A cut the caller certifies, such as a boundary of an exact pre-tokenizer, with the bytes a worker whose chunk
 *        ends there reads past it.
 *
 * The caller vouches for both sides of the cut: a worker that begins there segments the rest of the input alone as the
 * whole input does, and one whose chunk ends there, reading the context past it, segments the bytes below it as the
 * whole input does. Under byte-pair encoding a boundary of the input's encoding with no context is such a cut, by the
 * canonical adjacency theorem, while a positive context can end the worker's input inside a token.
 */
struct Supplied_cut
{
    /**
     * @brief The cut, the index of the first byte after it.
     */
    std::size_t position{};

    /**
     * @brief The bytes a worker reads past the cut, the input ending there or at its end.
     */
    std::size_t context{};
};

/**
 * @brief Marks the last chunk, which ends at the input's end and needs no cut.
 */
struct Input_end
{
};

/**
 * @brief A window of the input whose certificate places a cut: the token covering the window's last byte begins at the
 *        origin at every occurrence, so the cut lies at the window's first byte plus the origin.
 */
struct Certified_window
{
    /**
     * @brief The index of the window's first byte in the input.
     */
    std::size_t begin{};

    /**
     * @brief The window's width.
     */
    std::size_t width{};

    /**
     * @brief The certified origin, a byte index of the window.
     */
    std::size_t origin{};
};

/**
 * @brief The window of the pinned-byte theorem that places a cut: of width R + Q_s + 1, its byte R pinned at every
 *        occurrence at the start that byte has in the window's own encoding, so the cut lies at the window's first byte
 *        plus that start.
 */
struct Uniform_window
{
    /**
     * @brief The index of the window's first byte in the input.
     */
    std::size_t begin{};

    /**
     * @brief The window's width, R + Q_s + 1.
     */
    std::size_t width{};

    /**
     * @brief The pinned byte, R, an index of the window.
     */
    std::size_t byte{};

    /**
     * @brief The start of the pinned byte's token in the window's own encoding, at most the byte.
     */
    std::size_t start{};
};

/**
 * @brief A position of the input that no occurrence of a piece straddles, a boundary of every segmentation of the input
 *        into pieces of the set the plan reads, by the cut theorem of Piece_set.
 */
struct Straddle_free_cut
{
    /**
     * @brief The cut, the index of the first byte after it.
     */
    std::size_t position{};
};

/**
 * @brief What licenses the cut that ends a chunk: the input's end, a cut the caller supplies, a window certified on the
 *        input, the uniform window of the pinned-byte theorem, or a position no occurrence of a piece straddles.
 */
using Cut_evidence = std::variant<Input_end, Supplied_cut, Certified_window, Uniform_window, Straddle_free_cut>;

/**
 * @brief One worker's chunk: the bytes it owns, from its first to the cut that ends it, the bytes it reads, and what
 *        licenses that cut.
 */
struct Planned_chunk
{
    /**
     * @brief The index of the chunk's first byte, the previous chunk's cut or zero.
     */
    std::size_t begin{};

    /**
     * @brief The index after the chunk's last byte, the cut ending it or the input's size; the worker keeps the tokens
     *        below it.
     */
    std::size_t owned_end{};

    /**
     * @brief The index after the last byte the worker reads, at least the owned end and at most the input's size.
     */
    std::size_t read_end{};

    /**
     * @brief The evidence licensing the cut at the owned end.
     */
    Cut_evidence evidence{};
};

/**
 * @brief A chunk plan: the chunks in input order, together covering the input, and how often the search asked for a
 *        window decision and how many distinct windows it decided.
 */
struct Chunk_plan
{
    /**
     * @brief The chunks, the first beginning at zero, each beginning at the previous one's owned end, the last ending
     *        at the input's size; an empty input has one empty chunk.
     */
    std::vector<Planned_chunk> chunks{};

    /**
     * @brief The window decisions the search asked for, repeats included.
     */
    std::size_t windows_asked{};

    /**
     * @brief The distinct windows decided, every other ask answered from the cache.
     */
    std::size_t windows_decided{};
};

/**
 * @brief What a plan is asked for: the chunks aimed for, the widest window the search certifies on the input, and the
 *        cuts the caller supplies.
 */
struct Plan_request
{
    /**
     * @brief The number of chunks aimed for; fewer result where no cut is found, and zero behaves as one.
     */
    std::size_t chunks{};

    /**
     * @brief The widest window certified on the input, the context budget; zero certifies none.
     */
    std::size_t budget{};

    /**
     * @brief The cuts the caller certifies, ascending by position.
     */
    std::span<const Supplied_cut> supplied{};
};

/**
 * @brief The radii of the pinned-byte theorem for a table of finite origin radius: the origin radius and the right
 *        start radius, or ceilings on them.
 */
struct Uniform_radii
{
    /**
     * @brief The origin radius R, or a ceiling on it.
     */
    std::size_t radius{};

    /**
     * @brief The right start radius Q_s, or a ceiling on it.
     */
    std::size_t start_radius{};
};

/**
 * @brief Plans the chunks of an input for workers that each scan their chunk alone, over a verifier: each cut a
 *        boundary of the input's one segmentation at which the bytes on either side are segmented alone as the whole
 *        input segments them.
 *
 * The cuts are sought as core::Lexer::chunk_boundaries() seeks them: each interior cut is the first found at or after
 * the later of its equal-division target and one past the previous cut, and a target with none at or after it is
 * dropped, so that the chunk before it is scanned sequentially to the next cut or to the end, the fallback over a
 * stretch with no cut. From the floor the search walks the bytes of the input, and at each byte takes the first of the
 * routes that gives a cut at or after the floor. A supplied cut at the byte is taken as the caller certifies it, its
 * context read past it. Then the windows ending at the byte are tried from the narrowest, one byte, to the budget, each
 * beginning at or after the chunk's first byte, so that a worker that begins there still reads the window: pinned_cut()
 * at the window's last byte gives the certified origin, and dependence() must find every certified cut of the pair
 * independent, the prefix before it and the suffix from it each segmented alone as the whole string segments them,
 * which is the per-cut guarantee a sparse subset of an inventory's anchors needs, where chunk_dependence() speaks of
 * cutting at every anchor. With the prefix side at no byte past the cut, right_context() of such a pair has least
 * context zero, so a worker reads to its cut. A wider window ending at the same byte occurs wherever the narrower one
 * does and so certifies the same cut, which is why a window whose cut lies before the floor is not widened: the search
 * grows only the windows that fail. Each window is decided once per plan, by its bytes, the cache answering every
 * repeat.
 *
 * Each window decision is two searches over products with the verifier, pinned_cut() and dependence(), and the walk
 * asks at most the budget's number of windows per byte. The certified windows are the ones the input holds, so a
 * stretch with none is scanned by one worker whatever the table; no bound on the gap between cuts comes with them.
 * @param verifier The verifier, functional.
 * @param input The input.
 * @param request The chunks aimed for, the budget and the supplied cuts.
 * @return The chunks with the evidence of each cut, and the asks and decisions the search made.
 */
[[nodiscard]] Chunk_plan chunk_plan(const Verifier& verifier, std::string_view input, const Plan_request& request);

/**
 * @brief Plans the chunks of an input for workers that each encode their chunk alone, over the token automaton of a
 *        byte-pair table, with the uniform window of the pinned-byte theorem when the caller gives the radii.
 *
 * The cuts are sought as the verifier's plan seeks them, from each floor byte by byte, a supplied cut first, then the
 * windows ending at the byte from the narrowest to the budget, then the uniform window. A window is certified at the
 * start its last byte has in the window's own encoding when miscovering() on the automaton finds no miscovering there,
 * the only origin that can be certified, since the window occurs in itself. A certified window's cut is a boundary of
 * the input's encoding, and by the canonical adjacency theorem of the report The Left Ripple of Byte-Pair Encoding the
 * bytes on either side of a boundary are encoded alone as the whole input encodes them, so a worker reads to its cut
 * and a window may begin anywhere in the input. The uniform window is the pinned-byte theorem's: with R the origin
 * radius and Q_s the right start radius, or ceilings on them, the window of width R + Q_s + 1 whose byte R is the byte
 * walked pins that byte at the start it has in the window's own encoding, which places a cut within L - 1 bytes before
 * the byte, L the longest token; it is encoded and never decided, and it exists at every byte from R to the input's
 * size less Q_s + 1, so with the radii each cut lies within L - 1 bytes past its floor once the floor is at least R and
 * the input runs Q_s + L bytes past it. The radii are the caller's: ripple_radius() decides R, rule_ceilings() computes
 * Q_s and both ceilings, and with an infinite radius the table has no uniform window and the certified windows that
 * occur are its cuts.
 * @param automaton The token automaton of a byte table, as token_automaton(merges) builds it, its symbols the bytes.
 * @param input The input.
 * @param request The chunks aimed for, the budget and the supplied cuts.
 * @param radii The origin radius and the right start radius, or ceilings on them, std::nullopt for no uniform window.
 * @return The chunks with the evidence of each cut, and the asks and decisions the search made.
 * @throws std::invalid_argument If the automaton reads the characters of an alphabet, as token_automaton(merges,
 *         alphabet) builds it, rather than the bytes; the message names the number of symbols it reads.
 */
[[nodiscard]] Chunk_plan chunk_plan(
        const Token_automaton& automaton, std::string_view input, const Plan_request& request,
        std::optional<Uniform_radii> radii);

/**
 * @brief Plans the chunks of an input for workers that each segment their chunk alone, over a piece set: each cut a
 *        position no occurrence of a piece straddles, a boundary of every segmentation of the input into pieces at
 *        which a worker reads to its cut, or one the caller supplies.
 *
 * The cuts are sought as the verifier's plan seeks them, each interior cut the first found at or after the later of its
 * equal-division target and one past the previous cut, and a target with none at or after it is dropped, so that the
 * chunk before it is scanned sequentially to the next cut or to the end, the fallback over a stretch with no cut, such
 * as a run of one byte whose every interior position a piece of that byte straddles. At each position a supplied cut
 * comes first, as the other routes take it, with its context read past it: a boundary of an exact pre-tokenizer is a
 * cut that every segmenter over the set keeps when it segments the pre-tokenized text. Then the straddle-free positions
 * are the cuts; Piece_set::straddle_free() reads the whole input once, so no window is decided and the request's budget
 * is not read.
 *
 * Cut at straddle-free positions, by the cut theorem of Piece_set every segmentation of the input into pieces is a
 * concatenation of segmentations of the chunks, and every such concatenation is one, so no worker needs a byte past its
 * cut to emit a segmentation of the input, for every segmenter over the set. Under a score summed over the tokens the
 * optimal segmentations of the input are exactly the concatenations of optimal segmentations of the chunks, and a
 * forward Viterbi pass in exact arithmetic that replaces the best path at a position only on a strictly greater score
 * returns on each chunk alone what it returns on the input, ties included, since every path it compares past a cut
 * carries the same added constant, the best score at the cut. A segmenter whose path scores round, as SentencePiece's
 * float32 lattice does on long inputs, may choose between paths of nearly equal score otherwise on the whole input than
 * on its chunks, so the contract is the exact optimum under that tie rule.
 * @param set The piece set.
 * @param input The input.
 * @param request The chunks aimed for and the supplied cuts; the budget is not read.
 * @return The chunks with the evidence of each cut, every read end equal to its owned end but at a supplied cut, where
 *         it lies the cut's context past it, and no window asked or decided.
 */
[[nodiscard]] Chunk_plan chunk_plan(const Piece_set& set, std::string_view input, const Plan_request& request);

} // namespace munch::verifier

#endif // MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_CHUNK_PLAN_HPP
