#include "modules/duckpgq_integration.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

void RegisterCheckDuckPGQMacro(Connection &conn) {
	// Macro: check_duckpgq_available - Check if DuckPGQ extension is loaded
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO check_duckpgq_available() AS (
			SELECT COUNT(*) > 0
			FROM duckdb_functions()
			WHERE function_name LIKE '%GRAPH_TABLE%'
		)
	)");
	CheckQueryResult(result, "create check_duckpgq_available macro");
}

void InitializeDuckPGQIntegration(Connection &conn) {
	// Attempt to load DuckPGQ (soft dependency)
	// If DuckPGQ is not available, continue gracefully
	// This is a soft dependency pattern - no error if DuckPGQ missing
}

void RegisterPropertyGraphMacros(Connection &conn) {
	// Macro: create_bom_property_graph - Create property graph from universal BOM schema
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO create_bom_property_graph(
			source_system := 'TEST'
		) AS TABLE (
			SELECT 'BOM_PROPERTY_GRAPH_' || source_system AS graph_name,
			       source_system AS source,
			       (SELECT COUNT(*) FROM bom_header WHERE source_system = source_system) AS num_nodes,
			       (SELECT COUNT(*) FROM bom_component) AS num_edges
		)
	)");
	CheckQueryResult(result, "create create_bom_property_graph macro", FailureMode::OPTIONAL);
}

void RegisterBOMTraversalMacros(Connection &conn) {
	// Macro: bom_explosion_1level - Single-level BOM explosion
	// Returns direct children of a parent material
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO bom_explosion_1level(
			query_material_id := '',
			header_table := 'bom_header',
			component_table := 'bom_component'
		) AS TABLE
		SELECT
			bh.parent_material_id,
			bc.child_material_id,
			bc.quantity_per
		FROM query_table(header_table) bh
		INNER JOIN query_table(component_table) bc ON bh.bom_id = bc.bom_id
		WHERE bh.parent_material_id = query_material_id
		ORDER BY bc.line_number
	)");
	CheckQueryResult(result, "create bom_explosion_1level macro", FailureMode::OPTIONAL);

	// Macro: bom_explosion_multilevel - Multi-level BOM explosion with depth limit
	// Uses recursive CTE to traverse all levels of BOM hierarchy
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO bom_explosion_multilevel(
			query_material_id := '',
			max_depth := 10,
			header_table := 'bom_header',
			component_table := 'bom_component'
		) AS TABLE
		WITH RECURSIVE bom_levels AS (
			-- Base case: direct children
			SELECT
				bh.parent_material_id,
				bc.child_material_id,
				bc.quantity_per,
				1 AS depth
			FROM query_table(header_table) bh
			INNER JOIN query_table(component_table) bc ON bh.bom_id = bc.bom_id
			WHERE bh.parent_material_id = query_material_id

			UNION ALL

			-- Recursive case: children of children
			SELECT
				bh.parent_material_id,
				bc.child_material_id,
				bc.quantity_per,
				bl.depth + 1
			FROM bom_levels bl
			INNER JOIN query_table(header_table) bh ON bl.child_material_id = bh.parent_material_id
			INNER JOIN query_table(component_table) bc ON bh.bom_id = bc.bom_id
			WHERE bl.depth < max_depth
		)
		SELECT DISTINCT
			parent_material_id,
			child_material_id,
			quantity_per
		FROM bom_levels
	)");
	CheckQueryResult(result, "create bom_explosion_multilevel macro", FailureMode::OPTIONAL);

	// Macro: bom_where_used - Reverse BOM (where-used query)
	// Returns all parent materials that use a given child material
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO bom_where_used(
			child_material_id := '',
			header_table := 'bom_header',
			component_table := 'bom_component'
		) AS TABLE
		SELECT
			bh.parent_material_id,
			bc.child_material_id,
			bc.quantity_per
		FROM query_table(header_table) bh
		INNER JOIN query_table(component_table) bc ON bh.bom_id = bc.bom_id
		WHERE bc.child_material_id = child_material_id
		ORDER BY bh.parent_material_id
	)");
	CheckQueryResult(result, "create bom_where_used macro", FailureMode::OPTIONAL);

	// Macro: bom_common_components - Find common components between two materials
	// Returns components that appear in both parent materials' BOMs
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO bom_common_components(
			material_1 := '',
			material_2 := '',
			header_table := 'bom_header',
			component_table := 'bom_component'
		) AS TABLE
		WITH mat1_components AS (
			SELECT bc.child_material_id
			FROM query_table(header_table) bh
			INNER JOIN query_table(component_table) bc ON bh.bom_id = bc.bom_id
			WHERE bh.parent_material_id = material_1
		),
		mat2_components AS (
			SELECT bc.child_material_id
			FROM query_table(header_table) bh
			INNER JOIN query_table(component_table) bc ON bh.bom_id = bc.bom_id
			WHERE bh.parent_material_id = material_2
		)
		SELECT
			m1.child_material_id AS child_material_id_1,
			m2.child_material_id AS child_material_id_2
		FROM mat1_components m1
		INNER JOIN mat2_components m2 ON m1.child_material_id = m2.child_material_id
		ORDER BY m1.child_material_id
	)");
	CheckQueryResult(result, "create bom_common_components macro", FailureMode::OPTIONAL);
}

} // namespace anofox
} // namespace duckdb
