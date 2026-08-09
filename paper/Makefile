# Builds both technical reports. Requires a LaTeX toolchain:
#   sudo apt-get install -y texlive-latex-recommended texlive-fonts-recommended texlive-latex-extra latexmk
#
# The papers are sibling directories, each self-contained with its own sources, bibliography, and Makefile;
# figures/ and data/ beside them are shared evidence. The bibliographies are deliberately separate sets,
# independently verified with different artifact pins, and must not be merged.

PAPERS := split-points split-windows

.PHONY: all clean $(PAPERS)

all: $(PAPERS)

$(PAPERS):
	$(MAKE) -C $@

clean:
	for paper in $(PAPERS); do $(MAKE) -C $$paper clean; done
