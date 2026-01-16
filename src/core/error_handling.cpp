#include "core/error_handling.hpp"

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
			// Continue on error - optional feature not available
			break;
		case FailureMode::BEST_EFFORT:
			// Silently continue - experimental feature
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
