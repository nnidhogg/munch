#include "munch/regex/unicode.hpp"

#include <span>
#include <string_view>

#include "munch/regex/utf8.hpp"

namespace munch::regex::unicode
{
namespace
{
using utf8::Code_point_range;

#include "class_ranges.inc"
#include "xid_ranges.inc"

// The two generated files pin one database between them; a regeneration that moves one and not the other is refused
// at compile time rather than shipped as two Unicode versions under one version().
static_assert(unicode_version == class_unicode_version);

} // namespace

Regex xid_start()
{
    return utf8::ranges(xid_start_ranges);
}

Regex xid_continue()
{
    return utf8::ranges(xid_continue_ranges);
}

Regex decimal_digit()
{
    return utf8::ranges(decimal_digit_ranges);
}

Regex white_space()
{
    return utf8::ranges(white_space_ranges);
}

Regex word()
{
    return utf8::ranges(word_ranges);
}

std::span<const Code_point_range> ranges(const Property property) noexcept
{
    switch (property)
    {
    case Property::xid_start:
        return xid_start_ranges;
    case Property::xid_continue:
        return xid_continue_ranges;
    case Property::decimal_digit:
        return decimal_digit_ranges;
    case Property::white_space:
        return white_space_ranges;
    case Property::word:
        return word_ranges;
    }

    return {};
}

std::string_view version() noexcept
{
    return unicode_version;
}

} // namespace munch::regex::unicode
