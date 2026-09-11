//! log-lines.l written for logos: every line one record, the newline between them. The line's `[^\n]+` is a
//! dot-shaped repetition, which logos refuses unless it is allowed.

use logos::Logos;

#[derive(Logos, Debug, PartialEq)]
pub enum Token {
    #[regex(r"[^\n]+", allow_greedy = true)]
    Line,

    #[token("\n")]
    Newline,
}
