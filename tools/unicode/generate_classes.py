#!/usr/bin/env python3
"""Generates the code point tables behind the Unicode forms of \\d, \\s and \\w from the Unicode Character Database.

The three classes are the ones the regex crate gives those escapes in Unicode mode, which logos inherits: \\d is the
general category Nd, \\s is the White_Space property, and \\w is the union of Alphabetic, the mark categories M,
Nd, the connector punctuation Pc and Join_Control. Reads DerivedCoreProperties.txt, PropList.txt and
extracted/DerivedGeneralCategory.txt, published at https://www.unicode.org/Public/<version>/ucd/, and emits the
class_ranges.inc file included by libs/regex/src/unicode.cpp. The generated file is checked in, so building munch
never touches the network; rerun this script against a newer database to move the pinned Unicode version, together
with generate_xid.py, since unicode.cpp declares one version for both files.

Usage: tools/unicode/generate_classes.py <DerivedCoreProperties.txt> <PropList.txt> <DerivedGeneralCategory.txt> [output.inc]
"""

import hashlib
import re
import sys
from pathlib import Path


def parse_ranges(lines: list[str], property_name: str) -> list[tuple[int, int]]:
    """Extracts the code point ranges holding the given property or general category, unmerged."""
    pattern = re.compile(r"^([0-9A-F]+)(?:\.\.([0-9A-F]+))?\s*;\s*" + property_name + r"\s*#")

    ranges = []

    for line in lines:
        if match := pattern.match(line):
            first = int(match.group(1), 16)
            last = int(match.group(2), 16) if match.group(2) else first

            if not first <= last <= 0x10FFFF:
                raise ValueError(f"invalid range {first:X}..{last:X}")

            ranges.append((first, last))

    if not ranges:
        raise ValueError(f"no range holds {property_name}")

    return ranges


def merge(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """Sorts and merges ranges, overlapping ones included, since a union of properties may repeat a code point."""
    ranges = sorted(ranges)

    merged = [ranges[0]]

    for first, last in ranges[1:]:
        if first <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(merged[-1][1], last))
        else:
            merged.append((first, last))

    return merged


def emit_array(name: str, ranges: list[tuple[int, int]]) -> str:
    entries = "\n".join(f"        {{.first = 0x{first:X}, .last = 0x{last:X}}}," for first, last in ranges)

    return (
        f"constexpr std::array<Code_point_range, {len(ranges)}> {name}{{{{\n{entries}\n}}}};\n"
    )


def version_of(lines: list[str], stem: str) -> str:
    match = re.match(r"# " + stem + r"-(\d+\.\d+\.\d+)\.txt", lines[0])

    if not match:
        raise ValueError(f"first line does not name the {stem} database version")

    return match.group(1)


def main() -> None:
    if len(sys.argv) < 4:
        sys.exit(__doc__)

    raw = [Path(argument).read_bytes() for argument in sys.argv[1:4]]

    core, props, categories = (text.decode("utf-8").splitlines() for text in raw)

    output = Path(sys.argv[4] if len(sys.argv) > 4 else Path(__file__).parents[2] / "libs/regex/src/class_ranges.inc")

    versions = {version_of(core, "DerivedCoreProperties"), version_of(props, "PropList"),
                version_of(categories, "DerivedGeneralCategory")}

    if len(versions) != 1:
        raise ValueError(f"the three files name different database versions: {sorted(versions)}")

    version = versions.pop()

    decimal_digit = merge(parse_ranges(categories, "Nd"))
    white_space = merge(parse_ranges(props, "White_Space"))
    word = merge(parse_ranges(core, "Alphabetic") + parse_ranges(categories, "Mn") + parse_ranges(categories, "Mc")
                 + parse_ranges(categories, "Me") + decimal_digit + parse_ranges(categories, "Pc")
                 + parse_ranges(props, "Join_Control"))

    # The ASCII forms the byte reading already has must be the ASCII rows of these tables, or the escapes would
    # mean one thing under (?-u) and another under Unicode mode for the same ASCII input.
    def holds(ranges: list[tuple[int, int]], point: int) -> bool:
        return any(first <= point <= last for first, last in ranges)

    for point in range(0x80):
        if holds(decimal_digit, point) != (0x30 <= point <= 0x39):
            raise ValueError(f"Nd disagrees with [0-9] at U+{point:04X}")
        if holds(white_space, point) != (0x09 <= point <= 0x0D or point == 0x20):
            raise ValueError(f"White_Space disagrees with [\\t-\\r ] at U+{point:04X}")
        if holds(word, point) != (0x30 <= point <= 0x39 or 0x41 <= point <= 0x5A or 0x61 <= point <= 0x7A
                                  or point == 0x5F):
            raise ValueError(f"the word class disagrees with [0-9A-Za-z_] at U+{point:04X}")

    names = ["DerivedCoreProperties.txt", "PropList.txt", "extracted/DerivedGeneralCategory.txt"]

    sources = "\n".join(
        f"// Source: https://www.unicode.org/Public/{version}/ucd/{name}\n"
        f"// Source SHA-256: {hashlib.sha256(bytes_).hexdigest()}"
        for name, bytes_ in zip(names, raw)
    )

    header = (
        f"// Generated by tools/unicode/generate_classes.py from the Unicode {version} database; do not edit.\n"
        f"{sources}\n"
        "// The ranges derive from the Unicode Character Database, (c) Unicode, Inc., used under the Unicode\n"
        "// License v3; the complete notice is in THIRD_PARTY_NOTICES.md at the repository root.\n"
        "\n"
        f'constexpr std::string_view class_unicode_version{{"{version}"}};\n'
        "\n"
    )

    output.write_text(
        header + emit_array("decimal_digit_ranges", decimal_digit) + "\n" + emit_array("white_space_ranges", white_space)
        + "\n" + emit_array("word_ranges", word),
        encoding="utf-8",
        newline="\n",
    )

    def points(ranges: list[tuple[int, int]]) -> int:
        return sum(last - first + 1 for first, last in ranges)

    print(f"Unicode {version}: Nd {len(decimal_digit)} ranges ({points(decimal_digit)} code points), "
          f"White_Space {len(white_space)} ranges ({points(white_space)} code points), "
          f"word {len(word)} ranges ({points(word)} code points) -> {output}")


if __name__ == "__main__":
    main()
