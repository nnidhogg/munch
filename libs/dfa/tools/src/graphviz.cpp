#include "lexer/dfa/tools/graphviz.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <ranges>
#include <stdexcept>

namespace lexer::dfa::tools
{
void Graphviz::to_file(const Dfa& dfa, const std::filesystem::path& path)
{
    if (std::error_code ec; std::filesystem::create_directories(path.parent_path(), ec), ec)
    {
        throw std::runtime_error("Unable to create directories " + path.parent_path().string() + "; " + ec.message());
    }

    std::ofstream file{path, std::ios::out};

    if (!file)
    {
        throw std::runtime_error("Unable to create file " + path.string() + "; " + std::strerror(errno));
    }

    if (file << to_dot(dfa); !file)
    {
        throw std::runtime_error("Unable to write data to file " + path.string() + "; " + std::strerror(errno));
    }
}

std::string Graphviz::to_dot(const Dfa& dfa)
{
    std::ostringstream oss;
    oss << "digraph DFA {\n";
    oss << "    rankdir=LR;\n";
    oss << "    ratio=1.0;\n";
    oss << "    node [shape = circle];\n";

    for (const auto& [state, token] : dfa.accept_states())
    {
        oss << "    " << state << " [shape = doublecircle, label=\"" << state << " (" << token.id() << ")" << "\"];\n";
    }

    oss << "    __start__ [shape = none, label=\"\"];\n";
    oss << "    __start__ -> " << dfa.init_state() << ";\n";

    for (const auto& [key, state] : dfa.transitions())
    {
        const auto& [from_state, transition]{key};

        oss << "    " << from_state << " -> " << state << " [label = " << create_label(transition) << "];\n";
    }

    oss << "}\n";

    return oss.str();
}

std::string Graphviz::create_label(const Label& label)
{
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
        if (isprint(symbol))
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

} // namespace lexer::dfa::tools
