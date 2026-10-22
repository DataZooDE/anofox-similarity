#pragma once

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include <string>
#include <cstdio>

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Failure Mode Enum - Explicit error handling strategy
//------------------------------------------------------------------------------

enum class FailureMode {
	REQUIRED,   // Throw on error (critical infrastructure)
	OPTIONAL,   // Continue on error (optional features)
	BEST_EFFORT // Silently ignore errors (experimental features)
};

//------------------------------------------------------------------------------
// Query Result Checking - Standardized error handling
//------------------------------------------------------------------------------

// Check if a query execution was successful, with explicit failure mode control
// - REQUIRED: Throws InvalidInputException on error (default for critical operations)
// - OPTIONAL: Logs warning but continues (for optional features like VSS indexes)
// - BEST_EFFORT: Silently ignores errors (for experimental features like triggers)
//
// Template parameter T: QueryResult, MaterializedQueryResult, or similar types
template <typename T>
void CheckQueryResult(const unique_ptr<T> &result, const std::string &operation, FailureMode mode = FailureMode::REQUIRED) {
	if (result->HasError()) {
		switch (mode) {
		case FailureMode::REQUIRED:
			throw InvalidInputException("Failed to %s: %s", operation.c_str(), result->GetError().c_str());
		case FailureMode::OPTIONAL:
		case FailureMode::BEST_EFFORT:
			// Non-fatal: an optional/experimental feature could not be set up. This used to be
			// swallowed completely, which is how several documented functions silently failed to
			// register. In debug builds we surface it on stderr so regressions are caught early;
			// release builds still continue so a missing optional dependency never blocks load.
#ifndef NDEBUG
			fprintf(stderr, "[anofox_similarity] WARNING: failed to %s: %s\n", operation.c_str(),
			        result->GetError().c_str());
#endif
			break;
		}
	}
}
} // namespace anofox
} // namespace duckdb
