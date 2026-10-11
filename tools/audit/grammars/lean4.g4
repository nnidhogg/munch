// Lean 4's lexical structure as Lean's own scanner reads it, Lean.Parser.Basic, Lean.Parser.StrInterpolation and
// Init.Meta.Defs at lean4 commit 5045d0056413266e57c625dcd7c365b10e377c52 (v4.34.1): identifiers through Lean's
// isIdFirst and isIdRest, whose letter-like ranges and subscripts are Lean's own rather than XID, guillemet-quoted
// parts, the numerals of numberFnAux, character and string literals with their escapes, and the line comment. A block
// or doc comment is a mode pushed at every opener and popped at every closer, so that comments nest; a string is a mode
// of its own, and an interpolated string a mode whose brace pushes the default mode, where every brace pushes it again
// and every closing brace pops. Lean's token table grows with every notation, infix and syntax declaration, so the
// symbols here are the core's ASCII ones and any other character is a token of its own.
lexer grammar Lean4;

IDENTIFIER         : PART ('.' PART)* ;
NAME_LITERAL       : '`' PART ('.' PART)* ;
NUMBER             : DIGITS ('.' DIGITS)? ([eE] [+\-]? DIGITS)?
                   | '0' [bB] ('_'* [01])+
                   | '0' [oO] ('_'* [0-7])+
                   | '0' [xX] ('_'* HEX)+ ;
CHARACTER          : '\'' (~['\\] | '\r\n' | ESCAPE) '\'' ;
STRING_OPEN        : '"' -> pushMode(STRING) ;
INTERPOLATED_OPEN  : [fms] '!"' -> pushMode(INTERPOLATED) ;
DOC_COMMENT_OPEN   : '/--' -> pushMode(DOC_COMMENT) ;
MODULE_DOC_OPEN    : '/-!' -> pushMode(DOC_COMMENT) ;
BLOCK_COMMENT_OPEN : '/-' -> skip, pushMode(BLOCK_COMMENT) ;
LINE_COMMENT       : '--' ~[\n]* -> skip ;
WHITESPACE         : (' ' | '\r'? '\n')+ -> skip ;
LEFT_BRACE         : '{' -> pushMode(DEFAULT_MODE) ;
RIGHT_BRACE        : '}' -> popMode ;
SYMBOL             : '!=' | '#[' | '&&' | '&&&' | '*>' | '++' | '->' | '..' | '...' | '::' | ':=' | '<$>' | '<*'
                   | '<*>' | '<-' | '<->' | '<;>' | '<<<' | '<=' | '<|' | '<|>' | '==' | '=>' | '>=' | '>>' | '>>='
                   | '>>>' | '@[' | '^^^' | '|>' | '|>.' | '||' | '|||' | '~~~' ;
OTHER              : . ;

fragment PART        : ID_FIRST ID_REST* | '\u00AB' ~[\u00BB]* '\u00BB' ;
fragment ID_FIRST    : [a-zA-Z_] | LETTER_LIKE ;
fragment ID_REST     : [a-zA-Z0-9_'!?] | LETTER_LIKE | SUBSCRIPT ;
fragment LETTER_LIKE : [\u03B1-\u03BA\u03BC-\u03C9\u0391-\u039F\u03A1-\u03A2\u03A4-\u03A9\u03CA-\u03FB\u1F00-\u1FFE]
                     | [\u2100-\u214F\u{1D49C}-\u{1D59F}\u00C0-\u00D6\u00D8-\u00F6\u00F8-\u00FF\u0100-\u017F] ;
fragment SUBSCRIPT   : [\u2080-\u2089\u2090-\u209C\u1D62-\u1D6A\u2C7C] ;
fragment DIGITS      : [0-9] ('_'* [0-9])* ;
fragment HEX         : [0-9a-fA-F] ;
fragment ESCAPE      : '\\' (["\\'nrt] | 'x' HEX HEX | 'u' HEX HEX HEX HEX) ;
fragment GAP         : '\\' '\r'? '\n' [ \t\r]* ;

mode STRING;

STRING_TEXT  : (~["\\] | ESCAPE | GAP)+ ;
STRING_CLOSE : '"' -> popMode ;

mode INTERPOLATED;

INTERPOLATED_TEXT  : (~["\\{] | ESCAPE | '\\{' | GAP)+ ;
INTERPOLATED_BRACE : '{' -> pushMode(DEFAULT_MODE) ;
INTERPOLATED_CLOSE : '"' -> popMode ;

mode BLOCK_COMMENT;

BLOCK_COMMENT_NEST  : '/-' -> skip, pushMode(BLOCK_COMMENT) ;
BLOCK_COMMENT_CLOSE : '-/' -> skip, popMode ;
BLOCK_COMMENT_TEXT  : (~[/\-]+ | [/\-]) -> skip ;

mode DOC_COMMENT;

DOC_COMMENT_NEST  : '/-' -> pushMode(DOC_COMMENT) ;
DOC_COMMENT_CLOSE : '-/' -> popMode ;
DOC_COMMENT_TEXT  : ~[/\-]+ | [/\-] ;
