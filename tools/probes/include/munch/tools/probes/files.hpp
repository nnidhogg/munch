#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_FILES_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_FILES_HPP

#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief The files a probe reads and writes, read_bytes, files_under and Output_file.
 */
namespace munch::tools::probes
{
/**
 * @brief Reads a file whole, byte for byte; a read that fails part way yields the bytes read before it.
 * @param path The file to read.
 * @return The file's bytes, std::nullopt when it cannot be opened.
 */
[[nodiscard]] std::optional<std::string> read_bytes(const std::filesystem::path& path);

/**
 * @brief Lists the regular files under a directory and its subdirectories in sorted order, optionally only those of
 *        one extension; a directory that cannot be walked throws std::filesystem::filesystem_error.
 * @param root The directory walked.
 * @param extension The extension, dot included, a listed file must have, or std::nullopt to list every regular file.
 * @return The files' paths, sorted.
 */
[[nodiscard]] std::vector<std::filesystem::path> files_under(
        const std::filesystem::path& root, std::optional<std::string_view> extension);

/**
 * @brief A file a probe writes, owned and closed on every path: close() reports whether every write and the close
 *        succeeded, and a file nobody closed is closed when its owner goes.
 */
class Output_file
{
public:
    /**
     * @brief No file: is_open() is false and nothing is written.
     */
    Output_file() = default;

    /**
     * @brief Opens a file for writing, truncating it; is_open() says whether it opened.
     * @param path The file to write.
     */
    explicit Output_file(const std::filesystem::path& path);

    /**
     * @brief Whether a file is open.
     * @return True from a successful open until close().
     */
    [[nodiscard]] bool is_open() const noexcept;

    /**
     * @brief The open file's stream, for writing with the C formatted output functions.
     * @return The stream, null when no file is open.
     */
    [[nodiscard]] std::FILE* stream() const noexcept;

    /**
     * @brief Closes the open file, checking its error indicator and then the close; a file must be open.
     * @return True when no write failed and the close succeeded.
     */
    [[nodiscard]] bool close();

private:
    /**
     * @brief Closes a file whose owner goes without closing it.
     */
    struct Closer
    {
        /**
         * @brief Closes the file.
         * @param file The file to close.
         */
        void operator()(std::FILE* file) const noexcept;
    };

    /**
     * @brief The open file, null when none is open.
     */
    std::unique_ptr<std::FILE, Closer> file_;
};

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_FILES_HPP
