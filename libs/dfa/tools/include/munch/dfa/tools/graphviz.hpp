#ifndef MUNCH_LIBS_DFA_TOOLS_INCLUDE_MUNCH_DFA_TOOLS_GRAPHVIZ_HPP
#define MUNCH_LIBS_DFA_TOOLS_INCLUDE_MUNCH_DFA_TOOLS_GRAPHVIZ_HPP

#include <filesystem>
#include <string>

#include "munch/dfa/dfa.hpp"
#include "munch/dfa/label.hpp"

namespace munch::dfa::tools
{
/**
 * @brief Utility class for exporting DFA objects to Graphviz DOT format and files.
 *
 * Provides static methods to generate DOT representations and write them to files for visualization.
 */
class Graphviz
{
public:
    /**
     * @brief Writes the DOT representation of a DFA to a file.
     * @param dfa The DFA to export.
     * @param path The file path to write the DOT output to.
     */
    static void to_file(const Dfa& dfa, const std::filesystem::path& path);

    /**
     * @brief Generates the DOT representation of a DFA as a string.
     * @param dfa The DFA to export.
     * @return The DOT format string representing the DFA.
     */
    [[nodiscard]] static std::string to_dot(const Dfa& dfa);

private:
    /**
     * @brief Creates a DOT label for a DFA transition label.
     * @param label The DFA label.
     * @return The DOT label string.
     */
    [[nodiscard]] static std::string create_label(const Label& label);
};

} // namespace munch::dfa::tools

#endif // MUNCH_LIBS_DFA_TOOLS_INCLUDE_MUNCH_DFA_TOOLS_GRAPHVIZ_HPP
