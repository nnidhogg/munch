/* A log: every line one record, the newline between them. The simplest token set that certifies a byte. */

%option noyywrap

%%
[^\n]+       return LINE;
\n           return NEWLINE;
%%
