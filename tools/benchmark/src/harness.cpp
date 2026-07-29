#include "munch/tools/benchmark/harness.hpp"

#include "munch/core/builder.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/utf8.hpp"

namespace munch::tools::benchmark
{
core::Lexer build_lexer(const bool greek_identifiers)
{
    using namespace munch::regex;

    const auto letter{[greek_identifiers](Regex ascii) {
        return greek_identifiers ? choice(std::move(ascii), utf8::range(U'Α', U'ω')) : std::move(ascii);
    }};

    core::Builder builder;

    builder.add_token(plus(any_of(Set{' ', '\t', '\n'})), Token::whitespace, 2);

    builder.add_token(
            concat(letter(any_of(Set::alpha() + '_')), kleene(letter(any_of(Set::alphanum() + '_')))),
            Token::identifier, 2);

    builder.add_token(plus(any_of(Set::digits())), Token::number, 2);

    // Keywords outrank identifiers, exercising priority resolution on equal-length matches.
    builder.add_token(choice(text("if"), text("else"), text("while"), text("return"), text("int")), Token::keyword, 1);

    builder.add_token(
            choice(text("=="), text("!="), text("<="), text(">="), text("+"), text("-"), text("*"), text("/"),
                   text("="), text("<"), text(">")),
            Token::operator_, 2);

    builder.add_token(any_of(Set{'(', ')', '{', '}', ';', ','}), Token::punctuation, 2);

    return builder.build();
}

std::string generate_input(const std::size_t size, const std::span<const char* const> identifiers)
{
    std::string input;

    input.reserve(size + 128);

    // A fixed-seed linear congruential generator keeps the input identical across runs and builds.
    unsigned seed{12345};

    const auto random{[&seed] { return seed = seed * 1664525U + 1013904223U, seed >> 16U; }};

    while (input.size() < size)
    {
        input += "while (";
        input += identifiers[random() % identifiers.size()];
        input += " <= ";
        input += std::to_string(random() % 100000);
        input += ") { ";
        input += identifiers[random() % identifiers.size()];
        input += " = ";
        input += identifiers[random() % identifiers.size()];
        input += " + ";
        input += std::to_string(random() % 997);
        input += "; if (x1 != 42) { return counter; } }\n";
    }

    return input;
}

} // namespace munch::tools::benchmark
