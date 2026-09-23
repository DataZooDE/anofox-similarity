#include "modules/bom_utilities.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"
#include "datazoo_function_doc.hpp"

namespace ddoc = datazoo::doc;

namespace duckdb {
namespace anofox {

static const DefaultTableMacro AGGREGATE_MATERIAL_COMPONENTS_MACRO = {
    DEFAULT_SCHEMA,
    "aggregate_material_components",
    {nullptr},
    {{"bom_table", "'bom_items'"}, {"material_filter", "NULL"}, {nullptr, nullptr}},
    R"(
		WITH filtered AS (
			SELECT parent_id, child_id
			FROM query_table(bom_table)
			WHERE parent_id IS NOT NULL AND child_id IS NOT NULL
			  AND (material_filter IS NULL OR parent_id = ANY(material_filter))
		)
		SELECT parent_id AS material_id,
		       list(child_id ORDER BY child_id) AS components
		FROM filtered
		GROUP BY parent_id)"};

static const DefaultTableMacro FILTER_RECENT_MOVEMENTS_MACRO = {
    DEFAULT_SCHEMA,
    "filter_recent_movements",
    {nullptr},
    {{"movements_table", "'goods_movements'"}, {"time_window_days", "365"}, {"min_quantity", "0"}, {nullptr, nullptr}},
    R"(
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
		WHERE movement_date >= (SELECT MAX(movement_date) FROM filtered) - (INTERVAL '1 day' * COALESCE(time_window_days, 365)))"};

void RegisterBOMUtilityMacros(ExtensionLoader &loader) {
	ddoc::Registrar reg(loader, {"similarity", "bom"});
	reg.RegisterTableMacro(AGGREGATE_MATERIAL_COMPONENTS_MACRO,
	           {ddoc::Doc()
	                .Describe("Groups a BOM edge list into one row per parent material with its child components as a sorted list \u2014 the shape the Jaccard and cold-start functions expect. 'material_filter' restricts the result to the listed parents; NULL means all of them.")
	                .Example("SELECT * FROM aggregate_material_components(bom_table := 'bom_items')")});
	reg.RegisterTableMacro(FILTER_RECENT_MOVEMENTS_MACRO,
	           {ddoc::Doc()
	                .Describe("Filters a goods-movement table to a recent window and a minimum quantity. The window is anchored on the most recent movement date IN THE DATA rather than wall-clock today, which works without the ICU extension, is deterministic for tests, and is what planners want on historical ERP extracts where 'today' may be years after the last movement.")
	                .Example("SELECT * FROM filter_recent_movements(movements_table := 'goods_movements', time_window_days := 180)")});
}

} // namespace anofox
} // namespace duckdb
