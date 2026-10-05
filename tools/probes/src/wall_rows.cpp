#include "munch/tools/probes/wall_rows.hpp"

#include <string>

#include "grammars.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/probes/builder_dbg.hpp"
#include "munch/tools/probes/wall_table.hpp"

namespace munch::tools::probes
{
regex::Regex delimited(const char delimiter)
{
    using namespace regex;

    const auto quote{text(std::string{delimiter})};

    const auto body{kleene(any_of(Set::all() - Set{delimiter}))};

    return concat(quote, body, quote);
}

void add_gadget_tokens(core::Builder& builder)
{
    using namespace regex;

    builder.add_token(delimited('"'), Local::str, 1);

    builder.add_token(plus(any_of(Set::all() - Set{'"'})), Local::chunk, 2);
}

Table gadget()
{
    Builder_dbg builder{};

    add_gadget_tokens(builder);

    return extract(builder.dfa());
}

Table two_string()
{
    using namespace regex;

    Builder_dbg builder{};

    builder.add_token(delimited('"'), Local::str, 1);

    builder.add_token(delimited('`'), Local::tick, 1);

    builder.add_token(plus(any_of(Set::all() - Set{'"', '`'})), Local::chunk, 2);

    return extract(builder.dfa());
}

Table csv_row()
{
    using namespace regex;

    Builder_dbg builder{};

    const auto quote{text(R"(")")};

    const auto field_byte{choice(any_of(Set::all() - Set{'"'}), text(R"("")"))};

    builder.add_token(concat(quote, kleene(field_byte), quote), Local::quoted, 1);

    builder.add_token(plus(any_of(Set::all() - Set{'"', ',', '\n', '\r'})), Local::bare, 2);

    builder.add_token(text(","), Local::comma, 2);

    builder.add_token(concat(optional(text("\r")), text("\n")), Local::newline, 2);

    return extract(builder.dfa());
}

Table json_strict()
{
    Builder_dbg builder{};

    figures::json(builder);

    return extract(builder.dfa());
}

Table c_like_row()
{
    Builder_dbg builder{};

    figures::c_like(builder, false);

    return extract(builder.dfa());
}

Table rollback_family()
{
    using namespace regex;

    Builder_dbg builder{};

    builder.add_token(text("a"), Local::a, 1);

    builder.add_token(concat(text("a"), kleene(text("b")), text("c")), Local::abc, 1);

    builder.add_token(text("b"), Local::b, 1);

    builder.add_token(text("x"), Local::x, 1);

    return extract(builder.dfa());
}

} // namespace munch::tools::probes
