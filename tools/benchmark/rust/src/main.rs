//! Runs the engine comparison's tokenization job through Rust's lexer class: logos, the code-generating lexer, and
//! the dense DFAs of regex-automata, munch's nearest relative in the runtime-construction class.
//!
//! The corpus generator is a byte-identical port of tools/benchmark/src/harness.cpp, and the validation matches
//! tools/benchmark/src/compare.cpp: every driver's exact (kind, length) token stream is compared before timing,
//! so results are comparable across the two binaries: identical inputs, identical extracted information,
//! identical proof of agreement. Usage: cargo run --release -- [input size in MiB] [passes]

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

/// Walks the input with logos, feeding each token's kind and length to the sink; false on rejection.
fn scan_logos(input: &str, mut sink: impl FnMut(usize, usize)) -> bool {
    let mut lexer = Token::lexer(input);

    while let Some(result) = lexer.next() {
        match result {
            Ok(token) => sink(kind(token), lexer.span().len()),
            Err(_) => return false,
        }
    }

    true
}

/// The timed loop keeps its own body: routing it through scan_logos() would compute each token's span for a
/// sink that ignores it, which measured about ten percent on the dense corpus.
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

/// Collects the exact (kind, length) token stream, or None when logos rejected the input.
fn stream_logos(input: &str) -> Option<Vec<(usize, usize)>> {
    let mut stream = Vec::new();

    if scan_logos(input, |kind, length| stream.push((kind, length))) {
        Some(stream)
    } else {
        None
    }
}

/// The bytes this token set may be split at, established by manual analysis: each one is consumed only as the
/// first byte of a token, so a chunk boundary placed immediately before it cannot change the tokenization. munch
/// certifies the identical set automatically from its compiled transition table; logos offers no certification,
/// so the safety argument for this steelman lives in this comment instead ('=' is absent because it is the second
/// byte of ==, <=, >= and !=).
fn is_split_point(byte: u8) -> bool {
    matches!(
        byte,
        b'(' | b')' | b'{' | b'}' | b';' | b',' | b'+' | b'-' | b'*' | b'/' | b'<' | b'>' | b'!'
    )
}

/// Mirrors compare.cpp chunk_boundaries(): the split point nearest each equal-division offset.
fn chunk_boundaries(input: &[u8], chunks: usize) -> Vec<usize> {
    let mut boundaries = vec![0];

    for index in 1..chunks {
        let mut offset = index * input.len() / chunks;

        while offset < input.len() && !is_split_point(input[offset]) {
            offset += 1;
        }

        if offset > *boundaries.last().unwrap() && offset < input.len() {
            boundaries.push(offset);
        }
    }

    boundaries.push(input.len());
    boundaries
}

/// Raises 31 to the given power with wraparound, for splicing per-chunk checksums in stream order.
fn pow31(mut exponent: usize) -> usize {
    let mut result: usize = 1;
    let mut base: usize = 31;

    while exponent != 0 {
        if exponent & 1 != 0 {
            result = result.wrapping_mul(base);
        }

        base = base.wrapping_mul(base);
        exponent >>= 1;
    }

    result
}

/// Tokenizes the input in chunks split at the hand-verified split points, one thread per chunk, splicing the
/// per-chunk checksums in stream order so the tally validates against the serial scan exactly.
/// Collects the threaded scan's exact token stream, chunks spliced in input order.
fn stream_logos_threaded(input: &str, chunks: usize) -> Option<Vec<(usize, usize)>> {
    let boundaries = chunk_boundaries(input.as_bytes(), chunks);

    let mut total = Vec::new();

    for index in 0..boundaries.len() - 1 {
        total.extend(stream_logos(&input[boundaries[index]..boundaries[index + 1]])?);
    }

    Some(total)
}

fn run_logos_threaded(input: &str, chunks: usize) -> Tally {
    let boundaries = chunk_boundaries(input.as_bytes(), chunks);
    let mut tallies = vec![Tally::default(); boundaries.len() - 1];

    std::thread::scope(|scope| {
        for (index, tally) in tallies.iter_mut().enumerate() {
            let chunk = &input[boundaries[index]..boundaries[index + 1]];

            scope.spawn(move || {
                *tally = run_logos(chunk);
            });
        }
    });

    let mut total = Tally::default();

    for tally in &tallies {
        if tally.tokens == 0 {
            return Tally::default();
        }

        total.checksum = total
            .checksum
            .wrapping_mul(pow31(tally.tokens))
            .wrapping_add(tally.checksum);
        total.tokens += tally.tokens;
    }

    total
}

/// Walks the input through the search API, feeding each token's kind and length to the sink; false on rejection.
fn scan_regex_automata(dfa: &dense::DFA<Vec<u32>>, input: &str, mut sink: impl FnMut(usize, usize)) -> bool {
    let haystack = input.as_bytes();
    let mut offset = 0usize;

    while offset < haystack.len() {
        let search = Input::new(haystack).anchored(Anchored::Yes).range(offset..);

        let m = match dfa.try_search_fwd(&search) {
            Ok(Some(m)) if m.offset() > offset => m,
            _ => return false,
        };

        sink(KIND_OF_PATTERN[m.pattern().as_usize()], m.offset() - offset);

        offset = m.offset();
    }

    true
}

fn run_regex_automata(dfa: &dense::DFA<Vec<u32>>, input: &str) -> Tally {
    let mut tally = Tally::default();

    let ok = scan_regex_automata(dfa, input, |kind, _| {
        tally.checksum = tally.checksum.wrapping_mul(31).wrapping_add(kind);
        tally.tokens += 1;
    });

    if ok { tally } else { Tally::default() }
}

fn stream_regex_automata(dfa: &dense::DFA<Vec<u32>>, input: &str) -> Option<Vec<(usize, usize)>> {
    let mut stream = Vec::new();

    if scan_regex_automata(dfa, input, |kind, length| stream.push((kind, length))) {
        Some(stream)
    } else {
        None
    }
}

/// Tokenizes through the raw automaton walk from the Automaton trait's documentation: the start state resolved
/// once (valid here, as no pattern is context-sensitive), manual next_state() stepping, and the DFA's
/// delayed-by-one match reporting handled explicitly. This is the steelman driver: it strips the per-token search
/// API entry that run_regex_automata() pays.
fn scan_regex_automata_raw(
    dfa: &dense::DFA<Vec<u32>>,
    start: StateID,
    input: &str,
    mut sink: impl FnMut(usize, usize),
) -> bool {
    let haystack = input.as_bytes();
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
            return false;
        }

        sink(KIND_OF_PATTERN[last_pattern], last_end - offset);

        offset = last_end;
    }

    true
}

fn run_regex_automata_raw(dfa: &dense::DFA<Vec<u32>>, start: StateID, input: &str) -> Tally {
    let mut tally = Tally::default();

    let ok = scan_regex_automata_raw(dfa, start, input, |kind, _| {
        tally.checksum = tally.checksum.wrapping_mul(31).wrapping_add(kind);
        tally.tokens += 1;
    });

    if ok { tally } else { Tally::default() }
}

fn stream_regex_automata_raw(
    dfa: &dense::DFA<Vec<u32>>,
    start: StateID,
    input: &str,
) -> Option<Vec<(usize, usize)>> {
    let mut stream = Vec::new();

    if scan_regex_automata_raw(dfa, start, input, |kind, length| stream.push((kind, length))) {
        Some(stream)
    } else {
        None
    }
}

/// Mirrors tools/benchmark/src/harness.hpp measure(): a warmup pass, then best, median, and worst of the timed
/// passes, so the spread is visible instead of only the most favorable run.
fn measure(name: &str, bytes: usize, passes: usize, run: impl Fn() -> Tally) -> bool {
    let expected = run();

    if expected.tokens == 0 {
        println!("{name}: rejected the input");
        return false;
    }

    let mut seconds = Vec::with_capacity(passes);

    for _ in 0..passes {
        let start = Instant::now();
        let tally = run();
        let elapsed = start.elapsed().as_secs_f64();

        assert_eq!(tally, expected);

        seconds.push(elapsed);
    }

    seconds.sort_by(|a, b| a.partial_cmp(b).unwrap());

    let mib = bytes as f64 / (1024.0 * 1024.0);
    let median = (seconds[(seconds.len() - 1) / 2] + seconds[seconds.len() / 2]) / 2.0;

    println!(
        "{name:<16} {mib:.1} MiB, {} tokens, {passes} passes: best {:.1}, median {:.1}, worst {:.1} MiB/s",
        expected.tokens,
        mib / seconds[0],
        mib / median,
        mib / seconds[seconds.len() - 1]
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
        // The exact comparison runs once per corpus, before timing: every driver must reproduce logos's token
        // stream to the kind and the length. The timed loops keep only the light count-and-kind tally as a
        // per-pass sanity signal.
        let reference = match stream_logos(input) {
            Some(stream) if !stream.is_empty() => stream,
            _ => {
                println!("logos rejected the {corpus} corpus");
                std::process::exit(1);
            }
        };

        for (name, stream) in [
            ("logos-mt4", stream_logos_threaded(input, 4)),
            ("logos-mt8", stream_logos_threaded(input, 8)),
            ("regex-automata", stream_regex_automata(&dfa, input)),
            ("regex-automata-raw", stream_regex_automata_raw(&dfa, start, input)),
        ] {
            match stream {
                None => {
                    println!("{name}/{corpus}: rejected the input");
                    std::process::exit(1);
                }
                Some(stream) if stream != reference => {
                    let index = stream
                        .iter()
                        .zip(reference.iter())
                        .take_while(|(a, b)| a == b)
                        .count();
                    println!(
                        "{name}/{corpus}: token stream diverges from logos at token {index} ({} tokens vs {})",
                        stream.len(),
                        reference.len()
                    );
                    std::process::exit(1);
                }
                _ => {}
            }
        }

        println!(
            "corpus {corpus}: {:.2} bytes per token",
            input.len() as f64 / reference.len() as f64
        );

        ok &= measure("logos", input.len(), passes, || run_logos(input));

        ok &= measure("logos-mt4", input.len(), passes, || run_logos_threaded(input, 4));

        ok &= measure("logos-mt8", input.len(), passes, || run_logos_threaded(input, 8));

        ok &= measure("regex-automata", input.len(), passes, || {
            run_regex_automata(&dfa, input)
        });

        ok &= measure("regex-automata-raw", input.len(), passes, || {
            run_regex_automata_raw(&dfa, start, input)
        });
    }

    std::process::exit(if ok { 0 } else { 1 });
}
