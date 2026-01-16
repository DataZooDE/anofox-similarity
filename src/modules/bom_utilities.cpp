#include "modules/bom_utilities.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

void RegisterBOMUtilityMacros(Connection &conn) {
	// Helper 1: Aggregate material components into component lists
	// Purpose: Centralize duplicated BOM component aggregation logic
	// Replaces 4 instances of identical pattern in find_similar_materials,
	// cold_start_analogs, and refresh_dirty_embeddings
	auto agg_result = conn.Query(R"(
		CREATE OR REPLACE MACRO aggregate_material_components(
			bom_table := 'bom_items',
			material_filter := NULL
		) AS TABLE
		WITH filtered AS (
			SELECT parent_id, child_id
			FROM query_table(bom_table)
			WHERE material_filter IS NULL OR parent_id = ANY(material_filter)
		)
		SELECT parent_id AS material_id,
		       list(child_id ORDER BY child_id) AS components
		FROM filtered
		GROUP BY parent_id
	)");
	CheckQueryResult(agg_result, "create aggregate_material_components macro", FailureMode::REQUIRED);

	// Helper 2: Filter recent movements by time window and quantity
	// Purpose: Centralize duplicated time window filtering logic
	// Replaces 3+ instances in extract_ts_features and compute_transactional_embeddings
	auto filter_result = conn.Query(R"(
		CREATE OR REPLACE MACRO filter_recent_movements(
			movements_table := 'goods_movements',
			time_window_days := 365,
			min_quantity := 0
		) AS TABLE
		SELECT material_id, movement_date, quantity, movement_type
		FROM query_table(movements_table)
		WHERE movement_date >= CAST(CURRENT_TIMESTAMP AS DATE) - INTERVAL '1 day' * time_window_days
		  AND quantity IS NOT NULL
		  AND quantity > min_quantity
	)");
	CheckQueryResult(filter_result, "create filter_recent_movements macro", FailureMode::REQUIRED);
}

} // namespace anofox
} // namespace duckdb
