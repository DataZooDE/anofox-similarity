#include "modules/duckpgq_integration.hpp"
#include "core/error_handling.hpp"
#include "datazoo_function_doc.hpp"
#include "duckdb/main/connection.hpp"

namespace ddoc = datazoo::doc;

namespace duckdb {
namespace anofox {

// Macro: check_duckpgq_available - Check if DuckPGQ extension is loaded
static const DefaultMacro CHECK_DUCKPGQ_AVAILABLE_MACRO = {
    DEFAULT_SCHEMA, "check_duckpgq_available", {nullptr}, {{nullptr, nullptr}},
    R"( (
			SELECT COUNT(*) > 0
			FROM duckdb_functions()
			WHERE function_name LIKE '%GRAPH_TABLE%'
		))"};

void RegisterCheckDuckPGQMacro(ExtensionLoader &loader) {
	ddoc::Registrar reg(loader, {"similarity", "graph"});
	reg.RegisterMacro(CHECK_DUCKPGQ_AVAILABLE_MACRO,
	                  {ddoc::Doc()
	                       .Describe("Returns true when the DuckPGQ extension is loaded, so a caller can "
	                                 "choose the graph-based BOM traversal path over the SQL fallback. "
	                                 "DuckPGQ is an optional soft dependency and is never auto-installed.")
	                       .Example("SELECT check_duckpgq_available()")});
}

void InitializeDuckPGQIntegration(Connection &conn) {
	// DuckPGQ is an optional soft dependency — must be installed and loaded by the user
	// before using property graph features. Auto-installing crashes in some environments.
	(void)conn;
}

static const DefaultTableMacro CREATE_BOM_PROPERTY_GRAPH_MACRO = {
    DEFAULT_SCHEMA,
    "create_bom_property_graph",
    {nullptr},
    {{"source_system", "'TEST'"}, {nullptr, nullptr}},
    R"( (
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
		))"};

void RegisterPropertyGraphMacros(ExtensionLoader &loader) {
	ddoc::Registrar reg(loader, {"similarity", "graph"});
	reg.RegisterTableMacro(CREATE_BOM_PROPERTY_GRAPH_MACRO,
	           {ddoc::Doc()
	                .Describe("Summarises the BOM graph for one source system as (graph_name, source, num_nodes, num_edges), counting distinct parent materials in bom_header and the components beneath them.")
	                .Example("SELECT * FROM create_bom_property_graph(source_system := 'SAP')")});
}

// Macro: bom_explosion_1level - Single-level BOM explosion
// Returns direct children of a parent material
static const DefaultTableMacro BOM_EXPLOSION_1LEVEL_MACRO = {
    DEFAULT_SCHEMA,
    "bom_explosion_1level",
    {nullptr},
    {{"query_material_id", "''"},
     {"header_table", "'bom_header'"},
     {"component_table", "'bom_component'"},
     {nullptr, nullptr}},
    R"(
		SELECT
			bh.parent_material_id,
			bc.child_material_id,
			bc.quantity_per
		FROM query_table(header_table) bh
		INNER JOIN query_table(component_table) bc ON bh.bom_id = bc.bom_id
		WHERE bh.parent_material_id = query_material_id
		ORDER BY bc.child_material_id)"};

static const DefaultTableMacro BOM_EXPLOSION_MULTILEVEL_MACRO = {
    DEFAULT_SCHEMA,
    "bom_explosion_multilevel",
    {nullptr},
    {{"query_material_id", "''"}, {"max_depth", "10"}, {"header_table", "'bom_header'"}, {"component_table", "'bom_component'"}, {nullptr, nullptr}},
    R"(
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
		FROM bom_levels)"};

static const DefaultTableMacro BOM_WHERE_USED_MACRO = {
    DEFAULT_SCHEMA,
    "bom_where_used",
    {nullptr},
    {{"child_material_id", "''"}, {"header_table", "'bom_header'"}, {"component_table", "'bom_component'"}, {nullptr, nullptr}},
    R"(
		SELECT
			bh.parent_material_id,
			bc.child_material_id,
			bc.quantity_per
		FROM query_table(header_table) bh
		INNER JOIN query_table(component_table) bc ON bh.bom_id = bc.bom_id
		WHERE bc.child_material_id = child_material_id
		ORDER BY bh.parent_material_id)"};

static const DefaultTableMacro BOM_COMMON_COMPONENTS_MACRO = {
    DEFAULT_SCHEMA,
    "bom_common_components",
    {nullptr},
    {{"material_1", "''"}, {"material_2", "''"}, {"header_table", "'bom_header'"}, {"component_table", "'bom_component'"}, {nullptr, nullptr}},
    R"(
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
		ORDER BY m1.child_material_id)"};

void RegisterBOMTraversalMacros(ExtensionLoader &loader) {
	ddoc::Registrar reg(loader, {"similarity", "bom"});
	reg.RegisterTableMacro(BOM_EXPLOSION_1LEVEL_MACRO,
	           {ddoc::Doc()
	                .Describe("Direct children of one parent material: returns (parent_material_id, child_material_id, quantity_per) for the immediate level only. 'header_table' and 'component_table' name the tables to read, so the macro works on any schema matching the universal BOM shape.")
	                .Example("SELECT * FROM bom_explosion_1level(query_material_id := 'PUMP-A')")});
	reg.RegisterTableMacro(BOM_EXPLOSION_MULTILEVEL_MACRO,
	           {ddoc::Doc()
	                .Describe("Full multi-level BOM explosion under one parent, to 'max_depth' levels. Uses UNION rather than UNION ALL so a shared subassembly reached by many paths is expanded once -- no exponential blow-up on diamond BOMs -- and the depth gate is clamped to a hard ceiling of 64 so a cyclic BOM cannot iterate unbounded.")
	                .Example("SELECT * FROM bom_explosion_multilevel(query_material_id := 'PUMP-A', max_depth := 5)")});
	reg.RegisterTableMacro(BOM_WHERE_USED_MACRO,
	           {ddoc::Doc()
	                .Describe("Reverse BOM lookup: every parent material that consumes a given child, with the quantity each one uses.")
	                .Example("SELECT * FROM bom_where_used(child_material_id := 'SEAL-001')")});
	reg.RegisterTableMacro(BOM_COMMON_COMPONENTS_MACRO,
	           {ddoc::Doc()
	                .Describe("Components that appear in the BOMs of both given materials -- the overlap used to judge how interchangeable two assemblies are.")
	                .Example("SELECT * FROM bom_common_components(material_1 := 'PUMP-A', material_2 := 'PUMP-B')")});
}

} // namespace anofox
} // namespace duckdb
