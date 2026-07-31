# Builds the technical report. Requires a LaTeX toolchain:
#   sudo apt-get install -y texlive-latex-recommended texlive-fonts-recommended texlive-latex-extra latexmk

PAPER := split-points

.PHONY: all clean

all: $(PAPER).pdf

$(PAPER).pdf: $(PAPER).tex refs.bib
	latexmk -pdf -halt-on-error $(PAPER).tex

clean:
	latexmk -C
	rm -f $(PAPER).bbl
