# Builds the technical report. Requires a LaTeX toolchain:
#   sudo apt-get install -y texlive-latex-recommended texlive-fonts-recommended texlive-latex-extra latexmk

PAPER := split-points

# The figure PDFs are checked in, so building the paper needs no Graphviz and no compiler. Regenerating them does;
# see README.md.
FIGURES := $(wildcard figures/*.pdf)

.PHONY: all clean

all: $(PAPER).pdf

$(PAPER).pdf: $(PAPER).tex refs.bib $(FIGURES)
	latexmk -pdf -halt-on-error -Werror $(PAPER).tex

clean:
	latexmk -C
	rm -f $(PAPER).bbl
