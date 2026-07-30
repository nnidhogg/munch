#include "munch/regex/unicode.hpp"

#include <array>
#include <string_view>

#include "munch/regex/utf8.hpp"

namespace munch::regex::unicode
{
namespace
{
using utf8::Code_point_range;

#include "xid_ranges.inc"

} // namespace

Regex xid_start()
{
    return utf8::ranges(xid_start_ranges);
}

Regex xid_continue()
{
    return utf8::ranges(xid_continue_ranges);
}

std::string_view version() noexcept
{
    return unicode_version;
}

} // namespace munch::regex::unicode
