PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=privacy
EXT_CONFIG=${PROJ_DIR}extension_config.cmake
OVERRIDE_GIT_DESCRIBE ?= v2.0.0

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# C++ unit tests (plan traversal, parser and compiler helpers). The runner is built on platforms other than
# Windows and WebAssembly; `make test` runs it after the SQL tests when it exists.
CPP_TEST_RUNNER := build/release/extension/privacy/pac_test_runner
.PHONY: test_cpp
test_cpp:
	@if [ -x $(CPP_TEST_RUNNER) ]; then \
		echo $(CPP_TEST_RUNNER); \
		$(CPP_TEST_RUNNER); \
	else \
		echo "$(CPP_TEST_RUNNER) is not built; skipping the C++ unit tests"; \
	fi

test: test_cpp
