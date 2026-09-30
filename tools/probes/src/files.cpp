#include "munch/tools/probes/files.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace munch::tools::probes
{
std::optional<std::string> read_bytes(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};

    if (!in.is_open())
    {
        return std::nullopt;
    }

    std::ostringstream bytes;

    bytes << in.rdbuf();

    return std::move(bytes).str();
}

std::vector<std::filesystem::path> files_under(
        const std::filesystem::path& root, const std::optional<std::string_view> extension)
{
    std::vector<std::filesystem::path> files;

    for (const auto& entry : std::filesystem::recursive_directory_iterator{root})
    {
        if (entry.is_regular_file() && (!extension || entry.path().extension() == *extension))
        {
            files.push_back(entry.path());
        }
    }

    std::ranges::sort(files);

    return files;
}

Output_file::Output_file(const std::filesystem::path& path) : file_{std::fopen(path.c_str(), "w")}
{}

bool Output_file::is_open() const noexcept
{
    return file_ != nullptr;
}

std::FILE* Output_file::stream() const noexcept
{
    return file_.get();
}

bool Output_file::close()
{
    const auto healthy{std::ferror(file_.get()) == 0};

    const auto closed{std::fclose(file_.release()) == 0};

    return healthy && closed;
}

void Output_file::Closer::operator()(std::FILE* const file) const noexcept
{
    std::ignore = std::fclose(file);
}

} // namespace munch::tools::probes
