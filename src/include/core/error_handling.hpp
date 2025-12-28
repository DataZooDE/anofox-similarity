#pragma once

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include <string>

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Failure Mode Enum - Explicit error handling strategy
//------------------------------------------------------------------------------

enum class FailureMode {
	REQUIRED,     // Throw on error (critical infrastructure)
	OPTIONAL,     // Continue on error (optional features)
	BEST_EFFORT   // Silently ignore errors (experimental features)
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
template<typename T>
void CheckQueryResult(const unique_ptr<T> &result,
                      const std::string &operation,
                      FailureMode mode = FailureMode::REQUIRED);

} // namespace anofox
} // namespace duckdb
