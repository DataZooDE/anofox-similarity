#include "modules/statistics_functions.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

void RegisterStatisticsFunctions(Connection &conn) {
	// Phase 1.3: Statistics helper functions for z-score normalization
	// Goal: Eliminate 98+ repeated z-score normalization subquery patterns
	//
	// APPROACH: Instead of creating a single zscore_normalize function,
	// we restructure the transactional_embeddings macro to:
	// 1. Load statistics_lookup CTE once (already done)
	// 2. Refactor normalized_features CTE to compute all z-scores efficiently
	//    using JOINs with statistics_lookup instead of repeated subqueries
	//
	// This reduces code from 98+ individual normalization expressions to
	// efficient SQL using the pre-loaded statistics table.
	//
	// Implementation status:
	// ✓ Phase 1.1: aggregate_material_components helper macro
	// ✓ Phase 1.2: filter_recent_movements helper macro
	// ○ Phase 1.3: Optimize z-score normalization in transactional_embeddings.cpp
	//   (Deferred: Requires restructuring normalized_features CTE)
	//
	// Note: This is an infrastructure placeholder for future optimization.
	// The actual z-score refactoring will be done directly in the SQL
	// of transactional_embeddings.cpp as a restructuring of the
	// normalized_features CTE to use pre-loaded statistics more efficiently.
}

} // namespace anofox
} // namespace duckdb
