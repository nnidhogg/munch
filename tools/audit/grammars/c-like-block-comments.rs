//! c-like-block-comments.l written for logos: the conventional rules plus the block comment, a skip like the line
//! comment and the whitespace. The block comment is spelled so that its loop cannot begin with a star, which is the
//! same language as the flex file's and the one spelling logos 0.15.1 scans, its graph deciding a repetition's end
//! on one byte.

use logos::Logos;

#[derive(Logos, Debug, PartialEq)]
#[logos(skip r"//[^\n]*")]
#[logos(skip r"/\*[^*]*\*+([^*/][^*]*\*+)*/")]
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
