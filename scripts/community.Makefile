PROJ_DIR := $(abspath .)
EXTENSION_NAME := geosupport
EXTENSION_VERSION := 0.3.0
USE_UNSTABLE_C_API := 0
TARGET_DUCKDB_VERSION := v1.5.6
# Test the prerelease community target with a prerelease Python host too.
ifneq ($(filter v2.%,$(DUCKDB_VERSION)),)
DUCKDB_TEST_VERSION ?= main
endif

include extension-ci-tools/makefiles/c_api_extensions/base.Makefile
include extension-ci-tools/makefiles/c_api_extensions/c_cpp.Makefile

.PHONY: configure release debug test_release test_debug clean_all
# Sequence venv creation before the platform and version files, even with -j.
configure: venv
	$(MAKE) -f scripts/community.Makefile platform extension_version
release: build_extension_with_metadata_release
debug: build_extension_with_metadata_debug
# The container builds the library; the host owns the Python test environment.
# Do not try to run a host-created venv against a different container interpreter.
ifeq ($(LINUX_CI_IN_DOCKER),1)
PYTHON_VENV_BIN := python3
test_release test_debug:
	@echo "SQLLogicTests run on the host after the Linux container build"
else
# Unlike the upstream template, always run linux_amd64 tests on the host.
test_release: check_configure
	@temp=$$(mktemp -d); trap 'rm -rf "$$temp"' EXIT; \
	unset GEOSUPPORT_CATALOG; GEOSUPPORT_HOME="$$temp/store" $(TEST_RUNNER_RELEASE)
test_debug: check_configure
	@temp=$$(mktemp -d); trap 'rm -rf "$$temp"' EXIT; \
	unset GEOSUPPORT_CATALOG; GEOSUPPORT_HOME="$$temp/store" $(TEST_RUNNER_DEBUG)
endif
clean_all: clean_build clean_cmake clean_configure
