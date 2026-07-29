//! Runs the engine comparison's tokenization job through Rust's lexer class: logos, the code-generating lexer, and
//! the dense DFAs of regex-automata, munch's nearest relative in the runtime-construction class.
//!
//! The corpus generator is a byte-identical port of tools/benchmark/src/harness.cpp, and the tally (token count and
//! order-sensitive kind checksum, over the same kind values) matches tools/benchmark/src/compare.cpp, so results
//! are comparable across the two binaries: identical inputs, identical extracted information, validated the same
//! way. Usage: cargo run --release -- [input size in MiB] [passes]

use logos::Logos;
use regex_automata::dfa::{dense, Automaton, StartKind};
use regex_automata::util::primitives::StateID;
use regex_automata::{Anchored, Input};
use std::time::Instant;

#[derive(Logos, Clone, Copy, Debug, PartialEq)]
enum Token {
    #[regex(r"[ \t\n]+")]
    Whitespace,

    #[regex(r"[A-Za-z_][A-Za-z0-9_]*")]
    Identifier,

    #[regex(r"[0-9]+")]
    Number,

    #[token("if")]
    #[token("else")]
    #[token("while")]
    #[token("return")]
    #[token("int")]
    Keyword,

    #[token("==")]
    #[token("!=")]
    #[token("<=")]
    #[token(">=")]
    #[token("+")]
    #[token("-")]
    #[token("*")]
    #[token("/")]
    #[token("=")]
    #[token("<")]
    #[token(">")]
    Operator,

    #[token("(")]
    #[token(")")]
    #[token("{")]
    #[token("}")]
    #[token(";")]
    #[token(",")]
    Punctuation,
}

/// The harness Token values, keeping checksums comparable with the C++ comparison binary.
fn kind(token: Token) -> usize {
    match token {
        Token::Whitespace => 1,
        Token::Identifier => 2,
        Token::Number => 3,
        Token::Keyword => 4,
        Token::Operator => 5,
        Token::Punctuation => 6,
    }
}

/// The comparison pattern set for regex-automata, one pattern per kind; keywords precede the identifier pattern
/// because leftmost-first semantics break ties by pattern order where munch uses priorities.
const PATTERNS: [&str; 6] = [
    r"[ \t\n]+",
    r"if|else|while|return|int",
    r"[A-Za-z_][A-Za-z0-9_]*",
    r"[0-9]+",
    r"==|!=|<=|>=|[-+*/=<>]",
    r"[(){};,]",
];

/// The harness Token value of each pattern, indexed by pattern identifier.
const KIND_OF_PATTERN: [usize; 6] = [1, 4, 2, 3, 5, 6];

/// The outcome of tokenizing the whole input once, zeroed if the input was rejected; matches compare.cpp's Tally.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct Tally {
    tokens: usize,
    checksum: usize,
}

/// Byte-identical port of tools/benchmark/src/harness.cpp generate_input().
fn generate(size: usize) -> String {
    const IDENTIFIERS: [&str; 6] = ["foo", "bar_baz", "counter", "x1", "value2", "tmp"];

    let mut input = String::with_capacity(size + 128);
    let mut seed: u32 = 12345;

    let mut random = move || -> u32 {
        seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
        seed >> 16
    };

    while input.len() < size {
        input.push_str("while (");
        input.push_str(IDENTIFIERS[random() as usize % IDENTIFIERS.len()]);
        input.push_str(" <= ");
        input.push_str(&(random() % 100000).to_string());
        input.push_str(") { ");
        input.push_str(IDENTIFIERS[random() as usize % IDENTIFIERS.len()]);
        input.push_str(" = ");
        input.push_str(IDENTIFIERS[random() as usize % IDENTIFIERS.len()]);
        input.push_str(" + ");
        input.push_str(&(random() % 997).to_string());
        input.push_str("; if (x1 != 42) { return counter; } }\n");
    }

    input
}

/// Byte-identical port of harness.cpp generate_source_input(): realistic token lengths and indentation.
fn generate_source(size: usize) -> String {
    const IDENTIFIERS: [&str; 10] = [
        "configuration_manager",
        "total_element_count",
        "process_next_request",
        "buffer_capacity",
        "initialize_state_machine",
        "compute_partial_checksum",
        "validation_result",
        "iterator_position",
        "acc",
        "idx",
    ];

    let mut input = String::with_capacity(size + 256);
    let mut seed: u32 = 12345;

    let mut random = move || -> u32 {
        seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
        seed >> 16
    };

    while input.len() < size {
        input.push_str("while (");
        input.push_str(IDENTIFIERS[random() as usize % IDENTIFIERS.len()]);
        input.push_str(" <= ");
        input.push_str(&(random() % 10000000).to_string());
        input.push_str(") {\n    ");
        input.push_str(IDENTIFIERS[random() as usize % IDENTIFIERS.len()]);
        input.push_str(" = ");
        input.push_str(IDENTIFIERS[random() as usize % IDENTIFIERS.len()]);
        input.push_str(" * ");
        input.push_str(IDENTIFIERS[random() as usize % IDENTIFIERS.len()]);
        input.push_str(" + ");
        input.push_str(&(random() % 100000).to_string());
        input.push_str(";\n    if (");
        input.push_str(IDENTIFIERS[random() as usize % IDENTIFIERS.len()]);
        input.push_str(" != ");
        input.push_str(&(random() % 997).to_string());
        input.push_str(") { return ");
        input.push_str(IDENTIFIERS[random() as usize % IDENTIFIERS.len()]);
        input.push_str("; }\n}\n");
    }

    input
}

fn run_logos(input: &str) -> Tally {
    let mut lexer = Token::lexer(input);
    let mut tally = Tally::default();

    while let Some(result) = lexer.next() {
        match result {
            Ok(token) => {
                tally.checksum = tally.checksum.wrapping_mul(31).wrapping_add(kind(token));
                tally.tokens += 1;
            }
            Err(_) => return Tally::default(),
        }
    }

    tally
}

fn run_regex_automata(dfa: &dense::DFA<Vec<u32>>, input: &str) -> Tally {
    let haystack = input.as_bytes();
    let mut tally = Tally::default();
    let mut offset = 0usize;

    while offset < haystack.len() {
        let search = Input::new(haystack).anchored(Anchored::Yes).range(offset..);

        let m = match dfa.try_search_fwd(&search) {
            Ok(Some(m)) if m.offset() > offset => m,
            _ => return Tally::default(),
        };

        tally.checksum = tally
            .checksum
            .wrapping_mul(31)
            .wrapping_add(KIND_OF_PATTERN[m.pattern().as_usize()]);
        tally.tokens += 1;

        offset = m.offset();
    }

    tally
}

/// Tokenizes through the raw automaton walk from the Automaton trait's documentation: the start state resolved
/// once (valid here, as no pattern is context-sensitive), manual next_state() stepping, and the DFA's
/// delayed-by-one match reporting handled explicitly. This is the steelman driver: it strips the per-token search
/// API entry that run_regex_automata() pays.
fn run_regex_automata_raw(dfa: &dense::DFA<Vec<u32>>, start: StateID, input: &str) -> Tally {
    let haystack = input.as_bytes();
    let mut tally = Tally::default();
    let mut offset = 0usize;

    while offset < haystack.len() {
        let mut sid = start;
        let mut last_pattern = usize::MAX;
        let mut last_end = offset;
        let mut at = offset;

        while at < haystack.len() {
            sid = dfa.next_state(sid, haystack[at]);
            at += 1;

            if dfa.is_special_state(sid) {
                if dfa.is_match_state(sid) {
                    last_pattern = dfa.match_pattern(sid, 0).as_usize();
                    last_end = at - 1;
                } else if dfa.is_dead_state(sid) {
                    break;
                }
            }
        }

        if at == haystack.len() && !dfa.is_dead_state(sid) {
            let eoi = dfa.next_eoi_state(sid);

            if dfa.is_match_state(eoi) {
                last_pattern = dfa.match_pattern(eoi, 0).as_usize();
                last_end = at;
            }
        }

        if last_pattern == usize::MAX || last_end == offset {
            return Tally::default();
        }

        tally.checksum = tally
            .checksum
            .wrapping_mul(31)
            .wrapping_add(KIND_OF_PATTERN[last_pattern]);
        tally.tokens += 1;

        offset = last_end;
    }

    tally
}

/// Mirrors tools/benchmark/src/harness.hpp measure(): a warmup pass, then the best of the timed passes.
fn measure(name: &str, bytes: usize, passes: usize, run: impl Fn() -> Tally) -> bool {
    let expected = run();

    if expected.tokens == 0 {
        println!("{name}: rejected the input");
        return false;
    }

    let mut best = f64::INFINITY;

    for _ in 0..passes {
        let start = Instant::now();
        let tally = run();
        let elapsed = start.elapsed().as_secs_f64();

        assert_eq!(tally, expected);

        if elapsed < best {
            best = elapsed;
        }
    }

    let mib = bytes as f64 / (1024.0 * 1024.0);

    println!(
        "{name:<16} {mib:.1} MiB, {} tokens, best of {passes} passes: {:.1} MiB/s",
        expected.tokens,
        mib / best
    );

    true
}

fn main() {
    let mib: usize = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(15);
    let passes: usize = std::env::args().nth(2).and_then(|s| s.parse().ok()).unwrap_or(15);

    let corpora = [
        ("dense", generate(mib << 20)),
        ("source", generate_source(mib << 20)),
    ];

    let dfa = dense::Builder::new()
        .configure(dense::Config::new().start_kind(StartKind::Anchored))
        .build_many(&PATTERNS)
        .expect("the comparison patterns must compile");

    let start = dfa
        .start_state_forward(&Input::new("").anchored(Anchored::Yes))
        .expect("the anchored start state must resolve");

    let mut ok = true;

    for (corpus, input) in &corpora {
        // logos provides the reference tally; the other drivers must agree on every token's kind and boundary.
        let reference = run_logos(input);

        if reference.tokens == 0 {
            println!("logos rejected the {corpus} corpus");
            std::process::exit(1);
        }

        for (name, tally) in [
            ("regex-automata", run_regex_automata(&dfa, input)),
            ("regex-automata-raw", run_regex_automata_raw(&dfa, start, input)),
        ] {
            if tally != reference {
                println!(
                    "{name}/{corpus}: tokenization disagrees with logos ({} tokens, checksum {}; expected {}, {})",
                    tally.tokens, tally.checksum, reference.tokens, reference.checksum
                );
                std::process::exit(1);
            }
        }

        println!(
            "corpus {corpus}: {:.2} bytes per token",
            input.len() as f64 / reference.tokens as f64
        );

        ok &= measure("logos", input.len(), passes, || run_logos(input));

        ok &= measure("regex-automata", input.len(), passes, || {
            run_regex_automata(&dfa, input)
        });

        ok &= measure("regex-automata-raw", input.len(), passes, || {
            run_regex_automata_raw(&dfa, start, input)
        });
    }

    std::process::exit(if ok { 0 } else { 1 });
}
