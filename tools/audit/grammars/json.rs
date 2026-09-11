//! json.l written for logos: RFC 8259's lexical forms, the definitions declared as subpatterns under the names
//! the flex file gives them. The unescaped string character runs to U+10FFFF as the RFC has it, where the flex
//! file's byte class runs to 0xff; over valid UTF-8 the two admit the same strings.

use logos::Logos;

#[derive(Logos, Debug, PartialEq)]
#[logos(skip r"[ \t\n\r]+")]
#[logos(subpattern digit = r"[0-9]")]
#[logos(subpattern hex = r"[0-9a-fA-F]")]
#[logos(subpattern unescaped = r"[\x20-\x21\x23-\x5b\x5d-\u{10FFFF}]")]
#[logos(subpattern escape = r#"\\(["\\/bfnrt]|u(?&hex)(?&hex)(?&hex)(?&hex))"#)]
pub enum Token {
    #[regex(r"\{|\}|\[|\]|:|,")]
    Structural,

    #[regex("true|false|null")]
    Literal,

    #[regex(r"-?(0|[1-9](?&digit)*)(\.(?&digit)+)?([eE][-+]?(?&digit)+)?")]
    Number,

    #[regex(r#""((?&unescaped)|(?&escape))*""#)]
    String,
}
