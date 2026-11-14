#include "modules/universal_schema.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"
#include "datazoo_function_doc.hpp"

namespace ddoc = datazoo::doc;

namespace duckdb {
namespace anofox {

void CreateUniversalBOMSchema(Connection &conn, bool drop_existing) {
	// Optional: Drop existing tables for clean migration
	if (drop_existing) {
		auto drop_result = conn.Query(R"(
			DROP TABLE IF EXISTS bom_component CASCADE;
			DROP TABLE IF EXISTS bom_header CASCADE;
			DROP TABLE IF EXISTS materials CASCADE;
		)");
		CheckQueryResult(drop_result, "drop existing universal schema tables", FailureMode::OPTIONAL);
	}

	// Create materials table
	auto materials_result = conn.Query(R"(
		CREATE TABLE IF NOT EXISTS materials (
			material_id VARCHAR PRIMARY KEY,
			material_number VARCHAR UNIQUE NOT NULL,
			description VARCHAR(500),
			material_type VARCHAR(20),
			material_group VARCHAR(50),
			procurement_type VARCHAR(20),
			base_uom VARCHAR(10),
			weight DECIMAL(12,4),
			cost_per_unit DECIMAL(12,4),
			source_system VARCHAR(20),
			is_active BOOLEAN DEFAULT TRUE,
			created_at TIMESTAMP DEFAULT current_timestamp
		)
	)");
	CheckQueryResult(materials_result, "create materials table");

	// Create bom_header table
	auto header_result = conn.Query(R"(
				CREATE TABLE IF NOT EXISTS bom_header (
					bom_id VARCHAR PRIMARY KEY,
					source_system VARCHAR NOT NULL,
					source_bom_id VARCHAR,
					parent_material_id VARCHAR NOT NULL,
			bom_type VARCHAR,
			alternative_number VARCHAR,
			revision VARCHAR,
			base_quantity DECIMAL(18,6) DEFAULT 1,
			base_uom VARCHAR(10),
			valid_from DATE,
			valid_to DATE,
			plant_id VARCHAR(20),
			is_approved BOOLEAN DEFAULT FALSE,
			created_at TIMESTAMP DEFAULT current_timestamp,
			CONSTRAINT fk_parent FOREIGN KEY (parent_material_id)
				REFERENCES materials(material_id)
		)
	)");
	CheckQueryResult(header_result, "create bom_header table");

	// Create bom_component table
	auto component_result = conn.Query(R"(
		CREATE TABLE IF NOT EXISTS bom_component (
			component_id VARCHAR PRIMARY KEY,
			bom_id VARCHAR NOT NULL REFERENCES bom_header(bom_id),
			line_number INTEGER NOT NULL,
			child_material_id VARCHAR NOT NULL,
			quantity_per DECIMAL(18,6) NOT NULL,
			quantity_uom VARCHAR(10) NOT NULL,
			is_fixed_quantity BOOLEAN DEFAULT FALSE,
			scrap_percent DECIMAL(8,4) DEFAULT 0,
			effective_from DATE,
			effective_to DATE,
			component_type VARCHAR(20),
			supply_type VARCHAR(20),
			operation_sequence INTEGER,
			is_alternative BOOLEAN DEFAULT FALSE,
			alternative_group VARCHAR(20),
			created_at TIMESTAMP DEFAULT current_timestamp,
			CONSTRAINT fk_child FOREIGN KEY (child_material_id)
				REFERENCES materials(material_id)
		)
	)");
	CheckQueryResult(component_result, "create bom_component table");
}

static const DefaultTableMacro BOM_TO_ITEMS_MACRO = {
    DEFAULT_SCHEMA,
    "bom_to_items",
    {nullptr},
    {{"header_table", "'bom_header'"}, {"component_table", "'bom_component'"}, {nullptr, nullptr}},
    R"(
		SELECT
			h.parent_material_id AS parent_id,
			c.child_material_id AS child_id,
			c.quantity_per AS quantity
		FROM query_table(header_table) h
		JOIN query_table(component_table) c ON h.bom_id = c.bom_id
		-- All BOM rows are returned. (Previously this silently dropped every row whose header
		-- is_approved was not TRUE — including the schema's own DEFAULT FALSE — which lost data.
		-- Filter on is_approved yourself if you only want approved BOMs.))"};

// The inverse of bom_to_items, as three macros with the universal schema's columns and types
// (CreateUniversalBOMSchema above), so each result can be inserted into its table directly.
// items_table needs parent_id, child_id and quantity (NULL quantity counts as 1).
static const DefaultTableMacro ITEMS_TO_MATERIALS_MACRO = {
    DEFAULT_SCHEMA,
    "items_to_materials",
    {nullptr},
    {{"items_table", "'bom_items'"}, {"source_system", "'MIGRATED'"}, {nullptr, nullptr}},
    R"(
		SELECT
			m.material_id::VARCHAR AS material_id,
			m.material_id::VARCHAR AS material_number,
			NULL::VARCHAR AS description,
			NULL::VARCHAR AS material_type,
			NULL::VARCHAR AS material_group,
			NULL::VARCHAR AS procurement_type,
			'EA' AS base_uom,
			NULL::DECIMAL(12,4) AS weight,
			NULL::DECIMAL(12,4) AS cost_per_unit,
			source_system::VARCHAR AS source_system,
			TRUE AS is_active,
			CURRENT_TIMESTAMP::TIMESTAMP AS created_at
		FROM (
			SELECT parent_id AS material_id FROM query_table(items_table)
			UNION
			SELECT child_id FROM query_table(items_table)
		) m
		WHERE m.material_id IS NOT NULL)"};

static const DefaultTableMacro ITEMS_TO_BOM_HEADER_MACRO = {
    DEFAULT_SCHEMA,
    "items_to_bom_header",
    {nullptr},
    {{"items_table", "'bom_items'"}, {"source_system", "'MIGRATED'"}, {nullptr, nullptr}},
    R"(
		SELECT DISTINCT
			('BOM_' || parent_id)::VARCHAR AS bom_id,
			source_system::VARCHAR AS source_system,
			parent_id::VARCHAR AS source_bom_id,
			parent_id::VARCHAR AS parent_material_id,
			'MANUFACTURING' AS bom_type,
			'01' AS alternative_number,
			'MIGRATED' AS revision,
			1::DECIMAL(18,6) AS base_quantity,
			'EA' AS base_uom,
			NULL::DATE AS valid_from,
			NULL::DATE AS valid_to,
			NULL::VARCHAR AS plant_id,
			TRUE AS is_approved,
			CURRENT_TIMESTAMP::TIMESTAMP AS created_at
		FROM query_table(items_table)
		WHERE parent_id IS NOT NULL)"};

static const DefaultTableMacro ITEMS_TO_BOM_COMPONENT_MACRO = {
    DEFAULT_SCHEMA,
    "items_to_bom_component",
    {nullptr},
    {{"items_table", "'bom_items'"}, {nullptr, nullptr}},
    R"(
		SELECT
			('COMP_' || parent_id || '_' || ROW_NUMBER() OVER (PARTITION BY parent_id ORDER BY child_id))::VARCHAR AS component_id,
			('BOM_' || parent_id)::VARCHAR AS bom_id,
			ROW_NUMBER() OVER (PARTITION BY parent_id ORDER BY child_id)::INTEGER AS line_number,
			child_id::VARCHAR AS child_material_id,
			COALESCE(quantity, 1)::DECIMAL(18,6) AS quantity_per,
			'EA' AS quantity_uom,
			FALSE AS is_fixed_quantity,
			0::DECIMAL(8,4) AS scrap_percent,
			NULL::DATE AS effective_from,
			NULL::DATE AS effective_to,
			'STOCK' AS component_type,
			'PUSH' AS supply_type,
			NULL::INTEGER AS operation_sequence,
			FALSE AS is_alternative,
			NULL::VARCHAR AS alternative_group,
			CURRENT_TIMESTAMP::TIMESTAMP AS created_at
		FROM query_table(items_table)
		WHERE parent_id IS NOT NULL AND child_id IS NOT NULL)"};

void RegisterBOMConversionMacros(ExtensionLoader &loader) {
	ddoc::Registrar reg(loader, {"similarity", "erp"});
	reg.RegisterTableMacro(BOM_TO_ITEMS_MACRO,
	           {ddoc::Doc()
	                .Describe("Flattens the universal bom_header + bom_component pair into the flat (parent_id, child_id, quantity) bom_items shape that the similarity algorithms expect.")
	                .Example("SELECT * FROM bom_to_items(header_table := 'bom_header', component_table := 'bom_component')")});
	const char *items = " items_table needs parent_id, child_id and quantity (a NULL quantity counts as 1).";
	reg.RegisterTableMacro(ITEMS_TO_MATERIALS_MACRO,
	           {ddoc::Doc()
	                .Describe(string("The materials of a flat bom_items edge list (every parent and child, once), in the columns and types of the universal materials table, tagged with source_system.") + items)
	                .Example("SELECT * FROM items_to_materials(items_table := 'bom_items', source_system := 'MIGRATED')")});
	reg.RegisterTableMacro(ITEMS_TO_BOM_HEADER_MACRO,
	           {ddoc::Doc()
	                .Describe(string("One universal bom_header row per parent of a flat bom_items edge list (bom_id 'BOM_' || parent_id), tagged with source_system. With items_to_bom_component it is the inverse of bom_to_items.") + items)
	                .Example("SELECT * FROM items_to_bom_header(items_table := 'bom_items', source_system := 'MIGRATED')")});
	reg.RegisterTableMacro(ITEMS_TO_BOM_COMPONENT_MACRO,
	           {ddoc::Doc()
	                .Describe(string("One universal bom_component row per edge of a flat bom_items edge list, numbered per parent, quantity_per from quantity. Store it and items_to_bom_header as tables; bom_to_items over them gives the edges back.") + items)
	                .Example("SELECT * FROM items_to_bom_component(items_table := 'bom_items')")});
}

} // namespace anofox
} // namespace duckdb
