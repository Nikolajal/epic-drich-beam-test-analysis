# Makefile — build the tracking macros LaTeX report
#
# Usage:
#   make report               — generate .tex and compile (no plots)
#   make report PLOTS=<dir>   — include plots from directory
#   make clean

PLOTS  ?=
SCRIPT := generate_report.py
TEX    := tracking_report.tex
PDF    := tracking_report.pdf

.PHONY: report clean

report:
	python3 $(SCRIPT) $(PLOTS) > $(TEX)
	pdflatex -interaction=nonstopmode $(TEX)
	pdflatex -interaction=nonstopmode $(TEX)
	@echo "Done: $(PDF)"

clean:
	rm -f $(TEX) $(PDF) *.aux *.log *.toc *.lof *.lot *.out
