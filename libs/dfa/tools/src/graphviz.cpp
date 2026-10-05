#include "munch/dfa/tools/graphviz.hpp"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <tuple>

namespace munch::dfa::tools
{
void Graphviz::to_file(const Dfa& dfa, const std::filesystem::path& path)
{
    const auto directory{path.parent_path()};

    std::error_code error{};

    // A bare filename has an empty parent, and create_directories("") fails; only a stated directory is created.
    if (!directory.empty())
    {
        std::ignore = std::filesystem::create_directories(directory, error);
    }

    if (error)
    {
        const auto message{std::format("Unable to create directories {}; {}", directory.string(), error.message())};

        throw std::runtime_error{message};
    }

    std::ofstream file{path, std::ios::out};

    if (!file)
    {
        const auto message{std::format("Unable to create file {}; {}", path.string(), std::strerror(errno))};

        throw std::runtime_error{message};
    }

    file << to_dot(dfa);

    if (!file.flush())
    {
        const auto message{std::format("Unable to write data to file {}; {}", path.string(), std::strerror(errno))};

        throw std::runtime_error{message};
    }
}

std::string Graphviz::to_dot(const Dfa& dfa)
{
    std::ostringstream oss{};

    oss << R"(digraph DFA {
    rankdir=LR;
    ratio=1.0;
    node [shape = circle];
)";

    for (const auto& [state, token] : dfa.accept_states())
    {
        const auto node{
                std::format(R"dot(    {} [shape = doublecircle, label="{} ({})"];)dot", state, state, token.id())};

        oss << node << '\n';
    }

    oss << R"dot(    __start__ [shape = none, label=""];)dot" << '\n';

    oss << std::format("    __start__ -> {};\n", dfa.init_state());

    for (const auto& [key, state] : dfa.transitions())
    {
        const auto& [from_state, label]{key};

        const auto label_text{create_label(label)};

        oss << std::format("    {} -> {} [label = {}];\n", from_state, state, label_text);
    }

    oss << "}\n";

    return oss.str();
}

std::string Graphviz::create_label(const Label& label)
{
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

} // namespace munch::dfa::tools
