PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=anofox_similarity
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Override test targets to disable telemetry during test runs
# This prevents local tests and CI/CD from polluting PostHog telemetry data
test_release_internal:
	DATAZOO_DISABLE_TELEMETRY=1 ./build/release/test/unittest "test/*"

test_debug_internal:
	DATAZOO_DISABLE_TELEMETRY=1 ./build/debug/test/unittest "test/*"

test_reldebug_internal:
	DATAZOO_DISABLE_TELEMETRY=1 ./build/reldebug/test/unittest "test/*"