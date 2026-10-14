#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "munch/core/builder.hpp"
#include "munch/core/exceptions/state_limit_error.hpp"
#include "munch/core/lexer.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/dfa/label.hpp"
#include "munch/dfa/simulator.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/fuzz/reader.hpp"
#include "munch/verifier/piece_set.hpp"
#include "munch/verifier/verifier.hpp"
#include "munch/verifier/verifier_decisions.hpp"

namespace
{
using munch::tools::fuzz::Reader;
using munch::tools::fuzz::require;
using munch::verifier::Kept_token;
using munch::verifier::Marked;
using munch::verifier::Marked_string;
using munch::verifier::Verifier;

/**
 * @brief The bytes every fuzzed token, window and piece is written over, few enough that brute force stays cheap.
 */
constexpr std::string_view alphabet{"abc"};

/**
 * @brief The most DFA states a fuzzed token set may build into.
 */
constexpr std::size_t state_limit{64};

/**
 * @brief The most verifier states a fuzzed token set may have before its decisions are checked, since
 *        modulo_dependence()'s domain half is exponential in them.
 */
constexpr std::size_t verifier_state_limit{24};

/**
 * @brief The longest marked string the brute-force oracle enumerates.
 */
constexpr std::size_t longest_marked{7};

/**
 * @brief The widest window decoded for the verifier decisions.
 */
constexpr std::size_t widest_window{4};

/**
 * @brief The most pieces a fuzzed piece set holds.
 */
constexpr std::size_t most_pieces{4};

/**
 * @brief The longest piece of a fuzzed piece set.
 */
constexpr std::size_t longest_piece{3};

/**
 * @brief The widest window decoded for piece_cut().
 */
constexpr std::size_t widest_piece_window{5};

/**
 * @brief Decodes one byte of the alphabet.
 * @param reader The fuzz input.
 * @return The byte.
 */
char letter(Reader& reader)
{
    const auto index{reader.byte() % alphabet.size()};

    return alphabet[index];
}

/**
 * @brief Decodes a nonempty string over the alphabet of at most a length.
 * @param reader The fuzz input.
 * @param shortest The shortest length.
 * @param longest The longest length.
 * @return The string.
 */
std::string word(Reader& reader, const std::size_t shortest, const std::size_t longest)
{
    const auto length{shortest + reader.byte() % (longest - shortest + 1)};

    std::string decoded{};

    for (std::size_t index{0}; index < length; ++index)
    {
        decoded += letter(reader);
    }

    return decoded;
}

/**
 * @brief Decodes one byte-coded regex tree of bounded depth over the alphabet.
 *
 * At depth zero only leaves decode. Nullable patterns arise and are passed over by the caller, since armed_run()
 * refuses a DFA whose initial state accepts.
 * @param reader The fuzz input.
 * @param depth The deepest nesting left.
 * @return The regex.
 */
munch::regex::Regex make_regex(Reader& reader, const std::size_t depth)
{
    using namespace munch::regex;

    const auto kinds{depth == 0 ? 2U : 7U};

    // Decodes the left operand before the right, so an input yields one regex whichever order the compiler evaluates
    // arguments in.
    const auto operands{[&reader, depth] {
        auto left{make_regex(reader, depth - 1)};

        auto right{make_regex(reader, depth - 1)};

        return std::pair{std::move(left), std::move(right)};
    }};

    const auto kind{reader.byte() % kinds};

    switch (kind)
    {
    case 0:
        return text(letter(reader));
    case 1:
        return any_of(Set{letter(reader), letter(reader)});
    case 2:
    {
        auto [left, right]{operands()};

        return concat(std::move(left), std::move(right));
    }
    case 3:
    {
        auto [left, right]{operands()};

        return choice(std::move(left), std::move(right));
    }
    case 4:
    {
        auto operand{make_regex(reader, depth - 1)};

        return kleene(std::move(operand));
    }
    case 5:
    {
        auto operand{make_regex(reader, depth - 1)};

        return plus(std::move(operand));
    }
    default:
    {
        auto operand{make_regex(reader, depth - 1)};

        return optional(std::move(operand));
    }
    }
}

/**
 * @brief Decodes the token set into the builder: a count, then each token's expression before its priority byte.
 * @param reader The fuzz input.
 * @param builder The builder receiving the tokens.
 * @return The number of tokens added, their kinds running from zero.
 */
unsigned add_tokens(Reader& reader, munch::core::Builder& builder)
{
    const auto count{1U + reader.byte() % 3U};

    for (unsigned token{0}; token < count; ++token)
    {
        const auto regex{make_regex(reader, 3)};

        const auto priority{reader.byte() % 2U};

        builder.add_token(regex, token, priority);
    }

    return count;
}

/**
 * @brief Returns whether the verifier accepts a marked string, replayed through Verifier::step().
 * @param verifier The verifier.
 * @param marked The marked string.
 * @return True when it is accepted.
 */
bool is_accepted(const Verifier& verifier, const Marked_string& marked)
{
    if (marked.boundaries.size() != marked.bytes.size())
    {
        return false;
    }

    auto state{std::optional{verifier.start()}};

    for (std::size_t index{0}; index < marked.bytes.size() && state; ++index)
    {
        const Marked symbol{
                .byte = static_cast<unsigned char>(marked.bytes[index]),
                .boundary_after = marked.boundaries[index]};

        state = verifier.step(*state, symbol);
    }

    return state && verifier.accepts(*state);
}

/**
 * @brief Returns every accepted marked string over the alphabet up to the longest length, by a breadth-first walk over
 *        every marked symbol, so shorter strings come first.
 * @param verifier The verifier.
 * @return The accepted marked strings, lengths ascending.
 */
std::vector<Marked_string> accepted_strings(const Verifier& verifier)
{
    std::vector<std::pair<Verifier::State_t, Marked_string>> frontier{{verifier.start(), Marked_string{}}};

    std::vector<Marked_string> accepted{};

    for (std::size_t length{0}; length <= longest_marked; ++length)
    {
        std::vector<std::pair<Verifier::State_t, Marked_string>> next{};

        for (const auto& [state, marked] : frontier)
        {
            if (verifier.accepts(state))
            {
                accepted.push_back(marked);
            }

            if (length == longest_marked)
            {
                continue;
            }

            for (const auto byte : alphabet)
            {
                for (const auto boundary : {false, true})
                {
                    const Marked symbol{.byte = static_cast<unsigned char>(byte), .boundary_after = boundary};

                    const auto to{verifier.step(state, symbol)};

                    if (!to)
                    {
                        continue;
                    }

                    auto extended{marked};

                    extended.bytes += byte;
                    extended.boundaries.push_back(boundary);

                    next.emplace_back(*to, std::move(extended));
                }
            }
        }

        frontier = std::move(next);
    }

    return accepted;
}

/**
 * @brief Returns the verifier's accepted marking of an input, found by trying every marking a step admits.
 * @param verifier The verifier, functional.
 * @param bytes The input.
 * @return The accepted marked string, or std::nullopt when the input lies outside the domain.
 */
std::optional<Marked_string> marking_of(const Verifier& verifier, const std::string_view bytes)
{
    std::vector<std::pair<Verifier::State_t, std::vector<bool>>> live{{verifier.start(), {}}};

    for (const auto byte : bytes)
    {
        std::vector<std::pair<Verifier::State_t, std::vector<bool>>> next{};

        for (const auto& [state, boundaries] : live)
        {
            for (const auto boundary : {false, true})
            {
                const Marked symbol{.byte = static_cast<unsigned char>(byte), .boundary_after = boundary};

                const auto to{verifier.step(state, symbol)};

                if (!to)
                {
                    continue;
                }

                auto extended{boundaries};

                extended.push_back(boundary);

                next.emplace_back(*to, std::move(extended));
            }
        }

        live = std::move(next);
    }

    const auto accepting{std::ranges::find_if(live, [&verifier](const auto& entry) {
        const auto& [state, boundaries]{entry};

        return verifier.accepts(state);
    })};

    if (accepting == live.end())
    {
        return std::nullopt;
    }

    auto& [state, boundaries]{*accepting};

    return Marked_string{.bytes = std::string{bytes}, .boundaries = std::move(boundaries)};
}

/**
 * @brief Checks that the verifier is functional on the enumerated strings, no input accepted under two markings.
 * @param accepted The accepted marked strings, lengths ascending.
 */
void check_functional(const std::vector<Marked_string>& accepted)
{
    std::vector<std::string> inputs{};

    for (const auto& marked : accepted)
    {
        inputs.push_back(marked.bytes);
    }

    std::ranges::sort(inputs);

    const auto repeated{std::ranges::adjacent_find(inputs)};

    require(repeated == inputs.end());
}

/**
 * @brief Returns the start of the token covering a byte of a marked string.
 * @param marked The marked string.
 * @param index The byte.
 * @return The index of the token's first byte.
 */
std::size_t covering_boundary(const Marked_string& marked, std::size_t index)
{
    while (index > 0 && !marked.boundaries[index - 1])
    {
        --index;
    }

    return index;
}

/**
 * @brief Returns every occurrence of a window in a text, ascending.
 * @param text The text.
 * @param window The window, nonempty.
 * @return The index of each occurrence's first byte.
 */
std::vector<std::size_t> occurrences(const std::string_view text, const std::string_view window)
{
    std::vector<std::size_t> found{};

    for (auto at{text.find(window)}; at != std::string_view::npos; at = text.find(window, at + 1))
    {
        found.push_back(at);
    }

    return found;
}

/**
 * @brief Returns whether a marked string holds the window at an occurrence.
 * @param marked The marked string.
 * @param window The window.
 * @param occurrence The occurrence.
 * @return True when the window's bytes lie at the occurrence.
 */
bool holds_window(const Marked_string& marked, const std::string_view window, const std::size_t occurrence)
{
    return occurrence + window.size() <= marked.bytes.size() &&
           std::string_view{marked.bytes}.substr(occurrence, window.size()) == window;
}

/**
 * @brief Checks miscovering() against brute force: a certificate refused by no enumerated string, or a refutation that
 *        replays, its occurrence miscovered, as short as the shortest the enumeration refutes with.
 * @param verifier The verifier.
 * @param accepted The accepted marked strings, lengths ascending.
 * @param window The window, nonempty.
 * @param origin The origin, a byte index of the window.
 */
void check_miscovering(
        const Verifier& verifier, const std::vector<Marked_string>& accepted, const std::string_view window,
        const std::size_t origin)
{
    const auto last{window.size() - 1};

    const auto miscovered_at{[last, origin](const Marked_string& marked, const std::size_t at) {
        return covering_boundary(marked, at + last) != at + origin;
    }};

    const auto refutes{[&window, &miscovered_at](const Marked_string& marked) {
        const auto found{occurrences(marked.bytes, window)};

        return std::ranges::any_of(
                found, [&marked, &miscovered_at](const std::size_t at) { return miscovered_at(marked, at); });
    }};

    const auto shortest{std::ranges::find_if(accepted, refutes)};

    const auto found{munch::verifier::miscovering(verifier, window, origin)};

    if (!found)
    {
        require(shortest == accepted.end());

        return;
    }

    const auto& [segmentation, occurrence]{*found};

    require(is_accepted(verifier, segmentation));

    require(holds_window(segmentation, window, occurrence));

    require(miscovered_at(segmentation, occurrence));

    if (segmentation.bytes.size() > longest_marked)
    {
        require(shortest == accepted.end());

        return;
    }

    require(shortest != accepted.end() && shortest->bytes.size() == segmentation.bytes.size());
}

/**
 * @brief Returns the offsets from each occurrence of a window to the start of the token covering a byte of it, over
 *        every enumerated string up to a length, negative where the token begins before the occurrence.
 * @param accepted The accepted marked strings, lengths ascending.
 * @param window The window, nonempty.
 * @param byte The byte, an index of the window.
 * @param longest The longest string read.
 * @return The offsets, ascending and without repeats.
 */
std::vector<long> covering_offsets(
        const std::vector<Marked_string>& accepted, const std::string_view window, const std::size_t byte,
        const std::size_t longest)
{
    std::vector<long> offsets{};

    for (const auto& marked : accepted)
    {
        if (marked.bytes.size() > longest)
        {
            break;
        }

        for (const auto at : occurrences(marked.bytes, window))
        {
            const auto start{static_cast<long>(covering_boundary(marked, at + byte))};

            offsets.push_back(start - static_cast<long>(at));
        }
    }

    std::ranges::sort(offsets);

    const auto [first, last]{std::ranges::unique(offsets)};

    offsets.erase(first, last);

    return offsets;
}

/**
 * @brief Returns the offsets from the first occurrence of a window to the start of the token covering a byte of it,
 * over the shortest enumerated strings that hold the window, one of which pinned_cut() reads its offset from.
 * @param accepted The accepted marked strings, lengths ascending.
 * @param window The window, nonempty.
 * @param byte The byte, an index of the window.
 * @return The offsets, ascending and without repeats, empty when no enumerated string holds the window.
 */
std::vector<long> first_offsets(
        const std::vector<Marked_string>& accepted, const std::string_view window, const std::size_t byte)
{
    const auto holding{std::ranges::find_if(
            accepted, [window](const Marked_string& marked) { return marked.bytes.contains(window); })};

    std::vector<long> offsets{};

    for (auto at{holding}; at != accepted.end() && at->bytes.size() == holding->bytes.size(); ++at)
    {
        const auto first{at->bytes.find(window)};

        if (first == std::string::npos)
        {
            continue;
        }

        const auto start{static_cast<long>(covering_boundary(*at, first + byte))};

        offsets.push_back(start - static_cast<long>(first));
    }

    std::ranges::sort(offsets);

    const auto [first, last]{std::ranges::unique(offsets)};

    offsets.erase(first, last);

    return offsets;
}

/**
 * @brief Returns whether a set of covering offsets pins the byte: at most one offset, and that one inside the window.
 * @param offsets The offsets, ascending and without repeats.
 * @return True when no occurrence refutes a pin.
 */
bool pins(const std::vector<long>& offsets)
{
    return offsets.empty() || (offsets.size() == 1 && offsets.front() >= 0);
}

/**
 * @brief Checks pinned_cut() against brute force: an absent window occurring in no enumerated string, an offset every
 *        enumerated occurrence covers the byte from, or a refutation that replays, as short as the shortest length at
 *        which the enumerated occurrences stop pinning the byte, its occurrence breaking the pin.
 * @param verifier The verifier.
 * @param accepted The accepted marked strings, lengths ascending.
 * @param window The window, nonempty.
 * @param byte The byte, an index of the window.
 */
void check_pinned_cut(
        const Verifier& verifier, const std::vector<Marked_string>& accepted, const std::string_view window,
        const std::size_t byte)
{
    const auto offsets{covering_offsets(accepted, window, byte, longest_marked)};

    const auto found{munch::verifier::pinned_cut(verifier, window, byte)};

    if (std::holds_alternative<munch::verifier::Absent_window>(found))
    {
        require(offsets.empty());

        return;
    }

    if (const auto* const cut{std::get_if<std::size_t>(&found)})
    {
        require(*cut <= byte);

        require(offsets.empty() || offsets == std::vector{static_cast<long>(*cut)});

        return;
    }

    const auto& [segmentation, occurrence]{std::get<munch::verifier::Miscovering>(found)};

    require(is_accepted(verifier, segmentation));

    require(holds_window(segmentation, window, occurrence));

    const auto length{segmentation.bytes.size()};

    // A refutation longer than the enumeration's seven bytes is held only to the enumerated offsets pinning the byte,
    // so the occurrence check below covers witnesses up to seven bytes.
    if (length > longest_marked)
    {
        require(pins(offsets));

        return;
    }

    // The refutation is the shortest: the occurrences up to its length stop pinning the byte, and those shorter do not.
    const auto up_to_it{covering_offsets(accepted, window, byte, length)};

    const auto below_it{covering_offsets(accepted, window, byte, length - 1)};

    require(!pins(up_to_it) && pins(below_it));

    // The refuting occurrence breaks the pin: its token begins before it, or within it elsewhere than an offset read
    // first there, an offset before the occurrence never counting as one it disagrees with.
    const auto start{static_cast<long>(covering_boundary(segmentation, occurrence + byte))};

    const auto offset{start - static_cast<long>(occurrence)};

    const auto reference{first_offsets(accepted, window, byte)};

    require(offset < 0 ||
            std::ranges::any_of(reference, [offset](const long first) { return first >= 0 && first != offset; }));
}

/**
 * @brief Returns the kept segments of a marked string, a segment kept when the verifier records its kind and the kind
 *        is not discarded, at their positions shifted by an amount.
 * @param verifier The verifier.
 * @param marked The marked string, accepted.
 * @param discarded The discarded kinds.
 * @param shift The amount each position is shifted by.
 * @return The kept tokens, in order.
 */
std::vector<Kept_token> kept_tokens(
        const Verifier& verifier, const Marked_string& marked, const std::span<const std::size_t> discarded,
        const std::size_t shift)
{
    std::vector<Kept_token> kept{};

    const auto keep{[&kept, discarded, shift](
                            const std::optional<std::size_t> kind, const std::size_t begin, const std::size_t end) {
        if (!kind || std::ranges::contains(discarded, *kind))
        {
            return;
        }

        kept.push_back({.kind = *kind, .begin = begin + shift, .end = end + shift});
    }};

    auto state{verifier.start()};

    std::size_t begin{0};

    for (std::size_t index{0}; index < marked.bytes.size(); ++index)
    {
        const auto byte{static_cast<unsigned char>(marked.bytes[index])};

        const auto boundary{marked.boundaries[index]};

        if (boundary)
        {
            keep(verifier.close_kind(state, byte), begin, index + 1);

            begin = index + 1;
        }

        state = *verifier.step(state, {.byte = byte, .boundary_after = boundary});
    }

    if (!marked.bytes.empty())
    {
        keep(verifier.final_kind(state), begin, marked.bytes.size());
    }

    return kept;
}

/**
 * @brief What a cut of a marked string comes to modulo the discarded kinds: the sides outside the domain and, where
 *        both lie in it, the kept tokens of the string and of the two chunks.
 */
struct Cut_reading
{
    /**
     * @brief Whether the prefix before the cut lies outside the domain.
     */
    bool prefix_fails{};

    /**
     * @brief Whether the suffix from the cut lies outside the domain.
     */
    bool suffix_fails{};

    /**
     * @brief The kept tokens of the string, empty when a side lies outside the domain.
     */
    std::vector<Kept_token> whole_tokens{};

    /**
     * @brief The kept tokens of the prefix and then of the suffix, at their positions in the string, empty when a side
     *        lies outside the domain.
     */
    std::vector<Kept_token> chunk_tokens{};
};

/**
 * @brief Reads a cut of an accepted marked string by its definition, each side's marking found alone.
 * @param verifier The verifier.
 * @param marked The marked string, accepted.
 * @param cut The cut, at most the string's length.
 * @param discarded The discarded kinds.
 * @return The reading.
 */
Cut_reading read_cut(
        const Verifier& verifier, const Marked_string& marked, const std::size_t cut,
        const std::span<const std::size_t> discarded)
{
    const std::string_view bytes{marked.bytes};

    const auto prefix{marking_of(verifier, bytes.substr(0, cut))};

    const auto suffix{marking_of(verifier, bytes.substr(cut))};

    if (!prefix || !suffix)
    {
        return {.prefix_fails = !prefix, .suffix_fails = !suffix, .whole_tokens = {}, .chunk_tokens = {}};
    }

    auto chunk_tokens{kept_tokens(verifier, *prefix, discarded, 0)};

    const auto suffix_tokens{kept_tokens(verifier, *suffix, discarded, cut)};

    chunk_tokens.insert(chunk_tokens.end(), suffix_tokens.begin(), suffix_tokens.end());

    auto whole_tokens{kept_tokens(verifier, marked, discarded, 0)};

    return {.prefix_fails = false,
            .suffix_fails = false,
            .whole_tokens = std::move(whole_tokens),
            .chunk_tokens = std::move(chunk_tokens)};
}

/**
 * @brief Returns whether a reading refutes the cut: a side outside the domain, or the kept tokens moved.
 * @param reading The reading.
 * @return True when the cut is not independent modulo the discarded kinds.
 */
bool refutes(const Cut_reading& reading)
{
    return reading.prefix_fails || reading.suffix_fails || reading.whole_tokens != reading.chunk_tokens;
}

/**
 * @brief Checks modulo_dependence() against brute force: independence refused by no enumerated string, or a witness
 *        that replays, its sides and kept tokens those the definition reads, as short as the shortest the enumeration
 *        refutes with.
 * @param verifier The verifier.
 * @param accepted The accepted marked strings, lengths ascending.
 * @param window The window, nonempty.
 * @param offset The offset of the cut, a byte index of the window.
 * @param discarded The discarded kinds.
 */
void check_modulo_dependence(
        const Verifier& verifier, const std::vector<Marked_string>& accepted, const std::string_view window,
        const std::size_t offset, const std::span<const std::size_t> discarded)
{
    const auto string_refutes{[&](const Marked_string& marked) {
        const auto found{occurrences(marked.bytes, window)};

        return std::ranges::any_of(found, [&](const std::size_t at) {
            const auto reading{read_cut(verifier, marked, at + offset, discarded)};

            return refutes(reading);
        });
    }};

    const auto shortest{std::ranges::find_if(accepted, string_refutes)};

    const auto found{munch::verifier::modulo_dependence(verifier, window, offset, discarded)};

    if (!found)
    {
        require(shortest == accepted.end());

        return;
    }

    const auto& [segmentation, occurrence, cut, prefix_fails, suffix_fails, whole_tokens, chunk_tokens]{*found};

    require(is_accepted(verifier, segmentation));

    require(holds_window(segmentation, window, occurrence));

    require(cut == occurrence + offset);

    const auto reading{read_cut(verifier, segmentation, cut, discarded)};

    require(refutes(reading));

    require(prefix_fails == reading.prefix_fails && suffix_fails == reading.suffix_fails);

    require(whole_tokens == reading.whole_tokens && chunk_tokens == reading.chunk_tokens);

    if (segmentation.bytes.size() > longest_marked)
    {
        require(shortest == accepted.end());

        return;
    }

    require(shortest != accepted.end() && shortest->bytes.size() == segmentation.bytes.size());
}

/**
 * @brief Decodes the discarded kinds, a subset of the token set's kinds read from one byte's bits.
 * @param reader The fuzz input.
 * @param count The number of tokens in the set.
 * @return The discarded kinds, ascending.
 */
std::vector<std::size_t> discarded_kinds(Reader& reader, const unsigned count)
{
    const auto bits{reader.byte()};

    std::vector<std::size_t> discarded{};

    for (std::size_t kind{0}; kind < count; ++kind)
    {
        if (((bits >> kind) & 1U) != 0)
        {
            discarded.push_back(kind);
        }
    }

    return discarded;
}

/**
 * @brief Returns the DFA of a compiled token set read over the alphabet, as the audit reads one over its classes'
 *        bytes: its live states with the tokens they accept, and the step each takes on each byte into a live state.
 * @param simulator The compiled token set.
 * @return The DFA, its initial state the set's.
 */
munch::dfa::Dfa represented_dfa(const munch::dfa::Simulator& simulator)
{
    munch::dfa::Dfa::Transitions_t transitions{};

    munch::dfa::Dfa::Accept_states_t accepting{};

    for (std::size_t state{0}; state < simulator.state_count(); ++state)
    {
        if (!simulator.is_live(state))
        {
            continue;
        }

        if (const auto token{simulator.accepted(state)})
        {
            accepting.emplace(state, *token);
        }

        for (const auto symbol : alphabet)
        {
            const auto to{simulator.step(state, static_cast<unsigned char>(symbol))};

            if (!to || !simulator.is_live(*to))
            {
                continue;
            }

            const munch::dfa::Dfa::Key_t key{state, munch::dfa::Label{symbol}};

            transitions.emplace(key, *to);
        }
    }

    return {simulator.init_state(), std::move(transitions), std::move(accepting)};
}

/**
 * @brief Builds the armed-run verifier of a decoded token set and checks pinned_cut(), miscovering() and
 *        modulo_dependence() on a decoded window against the brute-force enumeration, a nullable or oversized set
 *        checking nothing.
 * @param reader The fuzz input.
 */
void check_verifier_decisions(Reader& reader)
{
    munch::core::Builder builder{};

    builder.set_state_limit(state_limit);

    const auto count{add_tokens(reader, builder)};

    const auto discarded{discarded_kinds(reader, count)};

    const auto window{word(reader, 1, widest_window)};

    const auto byte{reader.byte() % window.size()};

    const auto origin{reader.byte() % window.size()};

    const auto offset{reader.byte() % window.size()};

    const auto lexer{builder.build()};

    const auto dfa{represented_dfa(lexer.simulator())};

    if (dfa.has_accept_token(dfa.init_state()))
    {
        return;
    }

    const auto verifier{munch::verifier::armed_run(dfa)};

    if (verifier.state_count() > verifier_state_limit)
    {
        return;
    }

    const auto accepted{accepted_strings(verifier)};

    check_functional(accepted);

    check_miscovering(verifier, accepted, window, origin);

    check_pinned_cut(verifier, accepted, window, byte);

    check_modulo_dependence(verifier, accepted, window, offset, discarded);
}

/**
 * @brief Decodes a piece set: a count, then each piece, a spelling given twice kept once.
 * @param reader The fuzz input.
 * @return The pieces, in the order decoded.
 */
std::vector<std::string> decode_pieces(Reader& reader)
{
    const auto count{1 + reader.byte() % most_pieces};

    std::vector<std::string> pieces{};

    for (std::size_t index{0}; index < count; ++index)
    {
        auto piece{word(reader, 1, longest_piece)};

        if (std::ranges::contains(pieces, piece))
        {
            continue;
        }

        pieces.push_back(std::move(piece));
    }

    return pieces;
}

/**
 * @brief Returns every segmentation of a text into pieces, each as its boundary marks.
 * @param text The text.
 * @param pieces The pieces.
 * @return The segmentations.
 */
std::vector<std::vector<bool>> segmentations(const std::string_view text, const std::vector<std::string>& pieces)
{
    std::vector<std::pair<std::size_t, std::vector<bool>>> partial{{0, std::vector(text.size(), false)}};

    std::vector<std::vector<bool>> complete{};

    while (!partial.empty())
    {
        auto [position, marks]{std::move(partial.back())};

        partial.pop_back();

        if (position == text.size())
        {
            complete.push_back(std::move(marks));

            continue;
        }

        for (const auto& piece : pieces)
        {
            if (!text.substr(position).starts_with(piece))
            {
                continue;
            }

            const auto end{position + piece.size()};

            auto extended{marks};

            if (end < text.size())
            {
                extended[end - 1] = true;
            }

            partial.emplace_back(end, std::move(extended));
        }
    }

    return complete;
}

/**
 * @brief Returns whether some string over the alphabet up to the longest length, segmented into pieces, has no
 *        boundary at the window's cut at some occurrence.
 * @param pieces The pieces.
 * @param window The window.
 * @param offset The cut, between two of the window's bytes.
 * @return True when brute force refutes the cut.
 */
bool straddled_somewhere(
        const std::vector<std::string>& pieces, const std::string_view window, const std::size_t offset)
{
    std::vector<std::string> texts{std::string{}};

    for (std::size_t begin{0}; begin < texts.size(); ++begin)
    {
        const auto text{texts[begin]};

        if (text.size() < longest_marked)
        {
            for (const auto byte : alphabet)
            {
                texts.push_back(text + byte);
            }
        }

        const auto found{occurrences(text, window)};

        if (found.empty())
        {
            continue;
        }

        for (const auto& marks : segmentations(text, pieces))
        {
            const auto straddled{[&marks, offset](const std::size_t at) { return !marks[at + offset - 1]; }};

            if (std::ranges::any_of(found, straddled))
            {
                return true;
            }
        }
    }

    return false;
}

/**
 * @brief Checks piece_cut() on a decoded piece set and window: a certified cut refused by no enumerated segmentation,
 *        or a straddling witness that replays, its piece across the cut and every other segment one byte, which brute
 *        force refutes too whenever every byte of the alphabet is a piece.
 * @param reader The fuzz input.
 */
void check_piece_cut(Reader& reader)
{
    const auto pieces{decode_pieces(reader)};

    const auto window{word(reader, 2, widest_piece_window)};

    const auto offset{1 + reader.byte() % (window.size() - 1)};

    const auto set{munch::verifier::piece_set(pieces)};

    const auto found{munch::verifier::piece_cut(set, window, offset)};

    const auto refuted{straddled_somewhere(pieces, window, offset)};

    if (!found)
    {
        require(!refuted);

        return;
    }

    const auto& [segmentation, occurrence]{*found};

    const auto& [bytes, boundaries]{segmentation};

    require(boundaries.size() == bytes.size() && !bytes.empty() && !boundaries.back());

    require(holds_window(segmentation, window, occurrence));

    const auto cut{occurrence + offset};

    require(!boundaries[cut - 1]);

    std::size_t begin{0};

    for (std::size_t index{0}; index < bytes.size(); ++index)
    {
        if (index + 1 < bytes.size() && !boundaries[index])
        {
            continue;
        }

        const auto segment{bytes.substr(begin, index + 1 - begin)};

        const auto is_piece{std::ranges::contains(pieces, segment)};

        const auto across_cut{begin < cut && index + 1 > cut};

        require(across_cut ? is_piece : segment.size() == 1);

        begin = index + 1;
    }

    const auto every_byte_a_piece{std::ranges::all_of(
            alphabet, [&pieces](const char byte) { return std::ranges::contains(pieces, std::string{byte}); })};

    if (every_byte_a_piece && bytes.size() <= longest_marked)
    {
        require(refuted);
    }
}

} // namespace

/**
 * @brief Checks one fuzz input, the fuzz entry point run once per input.
 *
 * The input decodes a piece set and a window for piece_cut(), then a token set, compiled to its DFA and the DFA's
 * armed-run verifier, and a window for pinned_cut(), miscovering() and modulo_dependence(). Each decision is held to an
 * oracle that knows nothing of its search: every marked string over a three-byte alphabet up to seven bytes the
 * verifier accepts, or every segmentation into pieces of every such string, enumerated outright.
 * @param data The fuzz input.
 * @param size The input's size.
 * @return Zero; a violated invariant traps instead.
 */
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    Reader reader{std::span{data, size}};

    check_piece_cut(reader);

    try
    {
        check_verifier_decisions(reader);
    }
    catch (const munch::core::State_limit_error&)
    {
        // The state limit fired; the token set is passed over, as the builder harness passes it over.
    }

    return 0;
}
