#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/core/builder.hpp"
#include "munch/core/exceptions/state_limit_error.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"

namespace
{
/**
 * @brief Aborts on a violated invariant, which the fuzzer reports as a crash on this input.
 */
void require(const bool condition)
{
    if (!condition)
    {
        __builtin_trap();
    }
}

/**
 * @brief Reads the fuzz input as a byte stream, yielding zeros once exhausted.
 *
 * Exhaustion yielding zeros keeps every decode total: a truncated input decodes to a small grammar and an empty
 * scan instead of a rejected run, so the fuzzer never wastes inputs on decode failures.
 */
class Reader
{
public:
    Reader(const std::uint8_t* const data, const std::size_t size) : data_{data}, size_{size} {}

    [[nodiscard]] std::uint8_t byte() noexcept { return position_ < size_ ? data_[position_++] : 0; }

    /**
     * @brief The undecoded rest of the input, used as the text to scan.
     */
    [[nodiscard]] std::string remainder() const
    {
        return {reinterpret_cast<const char*>(data_) + position_, size_ - position_};
    }

private:
    const std::uint8_t* data_;

    std::size_t size_;

    std::size_t position_{0};
};

/**
 * @brief Decodes one byte-coded regex tree of bounded depth.
 *
 * At depth zero only leaves decode. Counted repetition expands the NFA by its count, so nesting compounds
 * multiplicatively; the library documents that bound as the caller's to enforce, and this harness enforces it by
 * allowing at most two nested ranges. Everything else, including nullable patterns such as a bare kleene, is
 * fair game.
 */
munch::regex::Regex make_regex(Reader& reader, const std::size_t depth, const std::size_t repeats = 2)
{
    using namespace munch::regex;

    switch (reader.byte() % (depth == 0 ? 2U : (repeats == 0 ? 7U : 8U)))
    {
    case 0:
        return text(static_cast<char>(reader.byte()));
    case 1:
        return any_of(Set{static_cast<char>(reader.byte()), static_cast<char>(reader.byte())});
    case 2:
        return concat(make_regex(reader, depth - 1, repeats), make_regex(reader, depth - 1, repeats));
    case 3:
        return choice(make_regex(reader, depth - 1, repeats), make_regex(reader, depth - 1, repeats));
    case 4:
        return kleene(make_regex(reader, depth - 1, repeats));
    case 5:
        return plus(make_regex(reader, depth - 1, repeats));
    case 6:
        return optional(make_regex(reader, depth - 1, repeats));
    default:
    {
        const auto min{reader.byte() % 3U};

        return range(make_regex(reader, depth - 1, repeats - 1), min, min + 1U + reader.byte() % 2U);
    }
    }
}

using Stream_t = std::vector<std::pair<unsigned, std::size_t>>;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    Reader reader{data, size};

    munch::core::Builder builder;

    // Fuzzed grammars are the untrusted-input case the limit exists for; it also keeps every build fast.
    builder.set_state_limit(256);

    const auto count{1U + reader.byte() % 4U};

    for (unsigned token{0}; token < count; ++token)
    {
        // Two priority levels make equal-priority ties and shadowed (dead) tokens common rather than rare.
        builder.add_token(make_regex(reader, 5), token, reader.byte() % 2U);
    }

    const auto chunks{reader.byte() % 5U};

    const auto input{reader.remainder()};

    try
    {
        const auto lexer{builder.build()};

        static_cast<void>(builder.diagnose());

        Stream_t serial;

        std::size_t total{0};

        const auto consumed{lexer.tokenize_all<unsigned>(input, [&](const unsigned token, const std::size_t length) {
            require(token < count);

            require(length > 0);

            serial.emplace_back(token, length);

            total += length;
        })};

        require(consumed <= input.size() && total == consumed);

        // The first token of the whole-input scan is exactly one longest-match attempt at offset zero.
        const auto first{lexer.tokenize<unsigned>(input)};

        require(first.length <= input.size());

        if (!serial.empty())
        {
            require(first.token == serial.front().first && first.length == serial.front().second);
        }

        const auto boundaries{lexer.chunk_boundaries(input, chunks)};

        require(boundaries.front() == 0 && boundaries.back() == input.size());

        for (std::size_t index{1}; index + 1 < boundaries.size(); ++index)
        {
            require(boundaries[index] > boundaries[index - 1] && boundaries[index] < input.size());

            require(lexer.is_split_point(input[boundaries[index]]));
        }

        // The explicit window planner: its cuts are shape-checked here, and each interior cut must be a
        // certified byte or the reported origin of a certified window found at most three bytes back. Its
        // stream guarantee is conditional on completely tokenizable input by documentation, so no stream
        // comparison is required on arbitrary fuzz input.
        const auto windowed{lexer.chunk_boundaries_with_windows(input, chunks)};

        require(windowed.front() == 0 && windowed.back() == input.size());

        for (std::size_t index{1}; index + 1 < windowed.size(); ++index)
        {
            require(windowed[index] > windowed[index - 1] && windowed[index] < input.size());

            const auto cut{windowed[index]};

            auto certified{lexer.is_split_point(input[cut])};

            for (std::size_t back{0}; !certified && back <= 3 && back <= cut; ++back)
            {
                const auto start{cut - back};

                for (auto length{std::max<std::size_t>(2, back + 1)};
                     !certified && length <= 4 && start + length <= input.size(); ++length)
                {
                    const auto origin{lexer.is_split_window(std::string_view{input}.substr(start, length))};

                    certified = origin.has_value() && *origin == back;
                }
            }

            require(certified);
        }

        // On completely tokenizable input the window theorem promises exact agreement for non-nullable token
        // sets, and a nullable set's plan here is its byte plan, which promises the same, so fuzz-generated
        // complete inputs hold the window plan to it: every windowed chunk consumes fully and the concatenation
        // equals the serial stream. On malformed input the windows contract deliberately promises nothing.
        if (consumed == input.size())
        {
            Stream_t rejoined;

            auto complete{true};

            for (std::size_t index{1}; index < windowed.size(); ++index)
            {
                const std::string_view chunk{input.data() + windowed[index - 1], windowed[index] - windowed[index - 1]};

                const auto part{lexer.tokenize_all<unsigned>(
                        chunk, [&rejoined](const unsigned token, const std::size_t length) {
                            rejoined.emplace_back(token, length);
                        })};

                complete = complete && part == chunk.size();
            }

            require(complete && rejoined == serial);
        }

        std::vector<Stream_t> streams(boundaries.size() - 1);

        const auto consumed_per_chunk{lexer.tokenize_all_parallel<unsigned>(
                input, chunks, [&streams](const std::size_t chunk, const unsigned token, const std::size_t length) {
                    streams[chunk].emplace_back(token, length);
                })};

        Stream_t parallel;

        for (const auto& stream : streams)
        {
            parallel.insert(parallel.end(), stream.begin(), stream.end());
        }

        // The serial stream is a prefix of the concatenated chunk streams, not always their equal: a serial scan
        // stops at the first unmatched offset while chunks past it still scan. On full consumption the two must
        // agree exactly, which is the certified split-point guarantee this harness exists to attack.
        require(parallel.size() >= serial.size());

        for (std::size_t index{0}; index < serial.size(); ++index)
        {
            require(parallel[index] == serial[index]);
        }

        if (consumed == input.size())
        {
            require(parallel.size() == serial.size());

            std::size_t sum{0};

            for (std::size_t chunk{0}; chunk < consumed_per_chunk.size(); ++chunk)
            {
                sum += consumed_per_chunk[chunk];

                require(consumed_per_chunk[chunk] == boundaries[chunk + 1] - boundaries[chunk]);
            }

            require(sum == input.size());
        }
    }
    catch (const munch::core::State_limit_error&)
    {
        // The state limit fired; rejecting the grammar is the documented behavior for untrusted token sets. Any
        // other exception escapes and counts as a finding.
    }

    return 0;
}
