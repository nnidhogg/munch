//! c-like-split-friendly.l written for logos: the conventional rules with the newline its own skip and the
//! whitespace run holding spaces and tabs only.

use logos::Logos;

#[derive(Logos, Debug, PartialEq)]
#[logos(skip("//[^\n]*", allow_greedy = true))]
#[logos(skip "\n")]
#[logos(skip r"[ \t]+")]
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
