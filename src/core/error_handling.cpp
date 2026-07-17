#include "core/error_handling.hpp"

#include <cstdio>

namespace duckdb {
namespace anofox {

// Template implementation for CheckQueryResult
template <typename T>
void CheckQueryResult(const unique_ptr<T> &result, const std::string &operation, FailureMode mode) {
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

// Explicit template instantiations for types used in this extension
template void CheckQueryResult<QueryResult>(const unique_ptr<QueryResult> &, const std::string &, FailureMode);
template void CheckQueryResult<MaterializedQueryResult>(const unique_ptr<MaterializedQueryResult> &,
                                                        const std::string &, FailureMode);

} // namespace anofox
} // namespace duckdb
