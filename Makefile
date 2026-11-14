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
# Report how much of the extension's catalog surface documents itself, as an
# agent querying duckdb_functions() would see it. Report-only: always exits 0.
#
# --preload vss because anofox_similarity autoloads it; without that its
# functions are counted as ours and the totals are wrong. The audit warns
# UNEXPECTED_AUTOLOAD when this list goes stale rather than reporting a
# silently inflated number.
.PHONY: doc-audit
doc-audit: release
	@datazoo-banner/audit-function-docs.sh \
	  --extension $(EXT_NAME) \
	  --artifact build/release/extension/$(EXT_NAME)/$(EXT_NAME).duckdb_extension \
	  --preload vss \
	  --check-examples

# Docs and Claude Code skills against the build: every ```sql block in README.md and
# plugins/anofox-similarity/skills runs, every function the extension registers is on an API
# page and named in a skill, and the generated API blocks are current. Fails on any problem.
# FILES=... additionally runs the SQL of those files (docs pages mostly need your own tables).
.PHONY: check-docs
check-docs: release
	@DATAZOO_DISABLE_TELEMETRY=1 python3 scripts/generate_api_reference.py --check
	@DATAZOO_DISABLE_TELEMETRY=1 python3 scripts/check_doc_snippets.py
	@$(if $(FILES),DATAZOO_DISABLE_TELEMETRY=1 python3 scripts/check_doc_snippets.py --out build/doc-snippets-files.tsv $(FILES))
