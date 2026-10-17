PYTHON ?= .venv/bin/python

.PHONY: build test integration
build:
	$(PYTHON) scripts/build.py
test: build
	$(PYTHON) -m pytest -q tests -m 'not integration'
integration: build
	$(PYTHON) -m pytest -q tests -m integration
