PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=privacy
EXT_CONFIG=${PROJ_DIR}extension_config.cmake
OVERRIDE_GIT_DESCRIBE ?= v2.0.0

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile
