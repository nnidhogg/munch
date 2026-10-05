#include "munch/nfa/tools/graphviz.hpp"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <tuple>

namespace munch::nfa::tools
{
void Graphviz::to_file(const Nfa& nfa, const std::filesystem::path& path)
{
    // A bare filename has an empty parent, and create_directories("") fails; only a stated directory is created.
    if (const auto directory{path.parent_path()}; !directory.empty())
    {
        std::error_code ec{};

        std::ignore = std::filesystem::create_directories(directory, ec);

        if (ec)
        {
            const auto message{std::format("Unable to create directories {}; {}", directory.string(), ec.message())};

            throw std::runtime_error{message};
        }
    }

    std::ofstream file{path, std::ios::out};

    if (!file)
    {
        const auto message{std::format("Unable to create file {}; {}", path.string(), std::strerror(errno))};

        throw std::runtime_error{message};
    }

    const auto dot{to_dot(nfa)};

    file << dot;

    if (!file.flush())
    {
        const auto message{std::format("Unable to write data to file {}; {}", path.string(), std::strerror(errno))};

        throw std::runtime_error{message};
    }
}

std::string Graphviz::to_dot(const Nfa& nfa)
{
    std::ostringstream oss{};

    oss << R"(digraph NFA {
    rankdir=LR;
    ratio=1.0;
    node [shape = circle];
)";

    const auto format_token{
            [](const std::optional<Token>& token) { return token.has_value() ? std::to_string(token->id()) : "n/a"; }};

    for (const auto& [state, token] : nfa.accept_states())
    {
        const auto token_label{format_token(token)};

        const auto node{
                std::format(R"dot(    {} [shape = doublecircle, label="{} ({})"];)dot", state, state, token_label)};

        oss << node << '\n';
    }

    oss << R"dot(    __start__ [shape = none, label=""];)dot" << '\n';

    oss << std::format("    __start__ -> {};\n", nfa.init_state());

    for (const auto& [key, states] : nfa.transitions())
    {
        const auto& [from_state, label]{key};

        const auto label_text{create_label(label)};

        for (const auto to_state : states)
        {
            oss << std::format("    {} -> {} [label = {}];\n", from_state, to_state, label_text);
        }
    }

    oss << "}\n";

    return oss.str();
}

std::string Graphviz::create_label(const Label& label)
{
    if (label.is_epsilon())
    {
        return R"("ε")";
    }

    std::ostringstream oss{};

    oss << '"';

    switch (const auto symbol{label.symbol()})
    {
    case '"':
        oss << R"(\")";
        break;
    case '\\':
        oss << R"(\\)";
        break;
    case '\n':
        oss << R"(\n)";
        break;
    case '\t':
        oss << R"(\t)";
        break;
    default:
    {
        const auto value{static_cast<unsigned char>(symbol)};

        if (std::isprint(value) != 0)
        {
            oss << symbol;

            break;
        }

        oss << std::format(R"(\x{:02X})", value);
    }
    }

    oss << '"';

    return oss.str();
}

} // namespace munch::nfa::tools
