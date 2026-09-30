#include "munch/tools/probes/wall_rows.hpp"

#include "grammars.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/probes/builder_dbg.hpp"
#include "munch/tools/probes/wall_table.hpp"

namespace munch::tools::probes
{
using namespace regex;

Table gadget()
{
    Builder_dbg builder{};

    builder.add_token(concat(text("\""), kleene(any_of(Set::all() - Set{'"'})), text("\"")), Local::Str, 1);

    builder.add_token(plus(any_of(Set::all() - Set{'"'})), Local::Chunk, 2);

    return extract(builder.dfa());
}

Table two_string()
{
    Builder_dbg builder{};

    builder.add_token(concat(text("\""), kleene(any_of(Set::all() - Set{'"'})), text("\"")), Local::Str, 1);

    builder.add_token(concat(text("`"), kleene(any_of(Set::all() - Set{'`'})), text("`")), Local::Tick, 1);

    builder.add_token(plus(any_of(Set::all() - Set{'"', '`'})), Local::Chunk, 2);

    return extract(builder.dfa());
}

Table csv_row()
{
    Builder_dbg builder{};

    builder.add_token(
            concat(text("\""), kleene(choice(any_of(Set::all() - Set{'"'}), text("\"\""))), text("\"")), Local::Quoted,
            1);

    builder.add_token(plus(any_of(Set::all() - Set{'"', ',', '\n', '\r'})), Local::Bare, 2);

    builder.add_token(text(","), Local::Comma, 2);

    builder.add_token(concat(optional(text("\r")), text("\n")), Local::Newline, 2);

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
    Builder_dbg builder{};

    builder.add_token(text("a"), Local::A, 1);

    builder.add_token(concat(text("a"), concat(kleene(text("b")), text("c"))), Local::Abc, 1);

    builder.add_token(text("b"), Local::B, 1);

    builder.add_token(text("x"), Local::X, 1);

    return extract(builder.dfa());
}

} // namespace munch::tools::probes
