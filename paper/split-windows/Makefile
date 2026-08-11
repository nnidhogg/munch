# Builds the technical report. Requires a LaTeX toolchain:
#   sudo apt-get install -y texlive-latex-recommended texlive-fonts-recommended texlive-latex-extra latexmk

PAPER := split-windows

# No figures yet; the evaluation tables are typeset from asserted artifact numbers.
FIGURES :=

.PHONY: all clean

all: $(PAPER).pdf

# LaTeX silently discards anything after \end{document}, so a botched edit can drop whole sections while every
# build check still passes. A merge did exactly that here once. Fail the build instead.
$(PAPER).pdf: $(PAPER).tex refs.bib 
	@awk '/\\end\{document\}/{f=1;next} f&&NF{print FILENAME": content after \\end{document} on line "NR": "$$0; c=1} END{exit c}' $(PAPER).tex
	latexmk -pdf -halt-on-error -Werror $(PAPER).tex

clean:
	latexmk -C
	rm -f $(PAPER).bbl
