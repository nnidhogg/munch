#include "munch/nfa/tools/graphviz.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <ranges>
#include <stdexcept>

namespace munch::nfa::tools
{
void Graphviz::to_file(const Nfa& nfa, const std::filesystem::path& path)
{
    // A bare filename has an empty parent, and create_directories("") fails; only a stated directory is created.
    if (std::error_code ec;
        !path.parent_path().empty() && (std::filesystem::create_directories(path.parent_path(), ec), ec))
    {
        throw std::runtime_error("Unable to create directories " + path.parent_path().string() + "; " + ec.message());
    }

    std::ofstream file{path, std::ios::out};

    if (!file)
    {
        throw std::runtime_error("Unable to create file " + path.string() + "; " + std::strerror(errno));
    }

    if (file << to_dot(nfa); !file.flush())
    {
        throw std::runtime_error("Unable to write data to file " + path.string() + "; " + std::strerror(errno));
    }
}

std::string Graphviz::to_dot(const Nfa& nfa)
{
    std::ostringstream oss;
    oss << "digraph NFA {\n";
    oss << "    rankdir=LR;\n";
    oss << "    ratio=1.0;\n";
    oss << "    node [shape = circle];\n";

    const auto format_token{[](const auto token) { return token.has_value() ? std::to_string(token->id()) : "n/a"; }};

    for (const auto& [state, token] : nfa.accept_states())
    {
        oss << "    " << state << " [shape = doublecircle, label=\"" << state << " (" << format_token(token) << ")"
            << "\"];\n";
    }

    oss << "    __start__ [shape = none, label=\"\"];\n";
    oss << "    __start__ -> " << nfa.init_state() << ";\n";

    for (const auto& [key, states] : nfa.transitions())
    {
        const auto& [from_state, transition]{key};

        std::ranges::for_each(states, [&oss, from_state, transition](const auto& to_state) {
            oss << "    " << from_state << " -> " << to_state << " [label = " << create_label(transition) << "];\n";
        });
    }

    oss << "}\n";

    return oss.str();
}

std::string Graphviz::create_label(const Label& label)
{
    if (label.is_epsilon())
    {
        return "\"ε\"";
    }

    std::ostringstream oss;

    oss << '"';

    switch (const auto symbol{label.symbol()})
    {
    case '\"':
        oss << "\\\"";
        break;
    case '\\':
        oss << "\\\\";
        break;
    case '\n':
        oss << "\\n";
        break;
    case '\t':
        oss << "\\t";
        break;
    default:
        if (isprint(static_cast<unsigned char>(symbol)))
        {
            oss << symbol;
        }
        else
        {
            oss << "\\x" << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
                << (static_cast<unsigned char>(symbol) & 0xFF);
        }
    }

    oss << '"';

    return oss.str();
}

} // namespace munch::nfa::tools
