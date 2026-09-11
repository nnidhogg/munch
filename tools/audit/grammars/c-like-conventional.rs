//! The conventional C-like tokenization of c-like-conventional.l, written for logos: the same seven rules, the
//! two discarding ones as skips. logos reads its patterns over scalars where flex reads bytes, so the string and
//! the comment admit every scalar the flex rules admit every byte of, and the two cut no input both tokenize
//! differently. The comment's `[^\n]*` is a dot-shaped repetition, which logos refuses unless it is allowed.

use logos::Logos;

#[derive(Logos, Debug, PartialEq)]
#[logos(skip("//[^\n]*", allow_greedy = true))]
#[logos(skip r"[ \t\n]+")]
pub enum Token {
    #[regex(r"[a-zA-Z_][a-zA-Z0-9_]*")]
    Identifier,

    #[regex(r"[0-9]+")]
    Number,

    #[regex(r"[-+*/<>=!&|^%~]")]
    Operator,

    #[regex(r"[\[\](){};,.:?]")]
    Punctuation,

    #[regex(r#""[^"\n]*""#)]
    String,
}
