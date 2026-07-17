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
	// DuckPGQ is an optional soft dependency — must be installed and loaded by the user
	// before using property graph features. Auto-installing crashes in some environments.
	(void)conn;
}

void RegisterPropertyGraphMacros(Connection &conn) {
	// Macro: create_bom_property_graph - Create property graph from universal BOM schema
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO create_bom_property_graph(
			source_system := 'TEST'
		) AS TABLE (
			-- NULL-default via CASE WHEN (NOT COALESCE): wrapping the source_system parameter in
			-- COALESCE(...) anywhere in this macro body makes DuckDB eagerly bind/typecheck the whole
			-- statement at CREATE-MACRO time, so the macro fails to register on any database where
			-- bom_header/bom_component do not already exist (i.e. essentially always, since this runs
			-- at extension load before user tables exist). CASE WHEN does not trigger that eager bind.
			SELECT 'BOM_PROPERTY_GRAPH_' || CASE WHEN source_system IS NULL THEN 'TEST' ELSE source_system END AS graph_name,
			       CASE WHEN source_system IS NULL THEN 'TEST' ELSE source_system END AS source,
			       -- Alias the table so bh.source_system is the COLUMN and the bare source_system is
			       -- the macro parameter; previously `source_system = source_system` was a tautology
			       -- (column shadowed the parameter) that counted every header regardless of source.
			       (SELECT COUNT(DISTINCT bh.parent_material_id)
			        FROM bom_header bh
			        WHERE bh.source_system = CASE WHEN source_system IS NULL THEN 'TEST' ELSE source_system END) AS num_nodes,
			       -- num_edges counts only components under headers of this source_system, to match num_nodes.
			       (SELECT COUNT(*)
			        FROM bom_component bc
			        JOIN bom_header bh ON bc.bom_id = bh.bom_id
			        WHERE bh.source_system = CASE WHEN source_system IS NULL THEN 'TEST' ELSE source_system END) AS num_edges
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
		ORDER BY bc.child_material_id
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
		-- UNION (not UNION ALL) makes the recursion set-based: identical (parent, child, quantity,
		-- depth) rows produced via different paths collapse to one, so a shared subassembly reached
		-- via many paths is expanded ONCE — no exponential blow-up on diamond BOMs. The depth gate is
		-- clamped to a hard ceiling so a cyclic BOM with a huge max_depth cannot iterate unbounded.
		WITH RECURSIVE bom_levels AS (
			SELECT
				bh.parent_material_id,
				bc.child_material_id,
				bc.quantity_per,
				1 AS depth
			FROM query_table(header_table) bh
			INNER JOIN query_table(component_table) bc ON bh.bom_id = bc.bom_id
			WHERE bh.parent_material_id = query_material_id

			UNION

			SELECT
				bh.parent_material_id,
				bc.child_material_id,
				bc.quantity_per,
				bl.depth + 1
			FROM bom_levels bl
			INNER JOIN query_table(header_table) bh ON bl.child_material_id = bh.parent_material_id
			INNER JOIN query_table(component_table) bc ON bh.bom_id = bc.bom_id
			-- COALESCE handles max_depth := NULL; LEAST(...,64) bounds cyclic traversal.
			WHERE bl.depth < LEAST(COALESCE(max_depth, 10), 64)
		)
		-- Edge-list semantics: each DISTINCT (parent, child, quantity_per) relationship once.
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
			SELECT DISTINCT bc.child_material_id
			FROM query_table(header_table) bh
			INNER JOIN query_table(component_table) bc ON bh.bom_id = bc.bom_id
			WHERE bh.parent_material_id = material_1
		),
		mat2_components AS (
			SELECT DISTINCT bc.child_material_id
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
