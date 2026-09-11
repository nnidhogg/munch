//! c-like-block-comments.l written for logos: the conventional rules plus the block comment, a skip like the line
//! comment and the whitespace.

use logos::Logos;

#[derive(Logos, Debug, PartialEq)]
#[logos(skip("//[^\n]*", allow_greedy = true))]
#[logos(skip r"/\*([^*]|\*+[^*/])*\*+/")]
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
