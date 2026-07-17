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
			WHERE parent_id IS NOT NULL AND child_id IS NOT NULL
			  AND (material_filter IS NULL OR parent_id = ANY(material_filter))
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
	// The time window is anchored on the most recent movement date IN THE DATA, not on the
	// wall-clock "today". This (a) works without the ICU extension, which this build requires
	// for current_date / now(), (b) is deterministic for tests, and (c) is what demand planners
	// actually want on historical ERP extracts, where "today" may be years after the last movement.
	// movement_type is intentionally NOT selected here so the helper works on the documented
	// (material_id, movement_date, quantity) schema; callers that need movement_type read it directly.
	auto filter_result = conn.Query(R"(
		CREATE OR REPLACE MACRO filter_recent_movements(
			movements_table := 'goods_movements',
			time_window_days := 365,
			min_quantity := 0
		) AS TABLE
		WITH src AS (
			-- Coerce movement_date/quantity so ERP extracts that ship dates as ISO or YYYYMMDD
			-- strings (and numeric quantities as text) work, instead of hitting a raw binder error.
			SELECT
				material_id,
				COALESCE(TRY_CAST(movement_date AS DATE),
				         TRY_STRPTIME(movement_date::VARCHAR, '%Y%m%d')::DATE) AS movement_date,
				TRY_CAST(quantity AS DOUBLE) AS quantity
			FROM query_table(movements_table)
			WHERE movement_date IS NOT NULL AND quantity IS NOT NULL
		),
		filtered AS (
			SELECT material_id, movement_date, quantity
			FROM src
			-- COALESCE(min_quantity, 0): an explicit NULL falls back to the default instead of making
			-- the predicate NULL (which would silently drop every row).
			WHERE movement_date IS NOT NULL AND quantity IS NOT NULL AND quantity > COALESCE(min_quantity, 0)
		)
		SELECT material_id, movement_date, quantity
		FROM filtered
		-- COALESCE(time_window_days, 365): same guard for the date-window predicate.
		WHERE movement_date >= (SELECT MAX(movement_date) FROM filtered) - (INTERVAL '1 day' * COALESCE(time_window_days, 365))
	)");
	CheckQueryResult(filter_result, "create filter_recent_movements macro", FailureMode::REQUIRED);
}

} // namespace anofox
} // namespace duckdb
