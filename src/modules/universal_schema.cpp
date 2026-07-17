#include "modules/universal_schema.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"

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

void RegisterBOMConversionMacros(Connection &conn) {
	// Macro: CreateUniversalBOMSchema - Create universal BOM schema tables
	// Provides SQL-callable interface to create materials, bom_header, bom_component tables
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO CreateUniversalBOMSchema() AS (
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
			);
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
				);
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
				);
			SELECT 'Schema creation completed' AS status
		)
	)");
	CheckQueryResult(result, "create CreateUniversalBOMSchema macro", FailureMode::OPTIONAL);

	// Macro: bom_to_items - Convert universal schema to flat bom_items format
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO bom_to_items(
			header_table := 'bom_header',
			component_table := 'bom_component'
		) AS TABLE
		SELECT
			h.parent_material_id AS parent_id,
			c.child_material_id AS child_id,
			c.quantity_per AS quantity
		FROM query_table(header_table) h
		JOIN query_table(component_table) c ON h.bom_id = c.bom_id
		-- All BOM rows are returned. (Previously this silently dropped every row whose header
		-- is_approved was not TRUE — including the schema's own DEFAULT FALSE — which lost data.
		-- Filter on is_approved yourself if you only want approved BOMs.)
	)");
	CheckQueryResult(result, "create bom_to_items macro");

	// Macro: items_to_bom - Convert flat bom_items to universal schema
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO items_to_bom(
			items_table := 'bom_items',
			source_system := 'MIGRATED'
		) AS TABLE
		WITH
		unique_materials AS (
			SELECT DISTINCT parent_id AS material_id FROM query_table(items_table)
			UNION
			SELECT DISTINCT child_id AS material_id FROM query_table(items_table)
		),
		bom_headers AS (
			SELECT DISTINCT
				'BOM_' || parent_id AS bom_id,
				source_system,
				parent_id AS source_bom_id,
				parent_id AS parent_material_id,
				'MANUFACTURING' AS bom_type,
				'01' AS alternative_number,
				'MIGRATED' AS revision,
				1 AS base_quantity,
				'EA' AS base_uom,
				DATE '2024-01-01' AS valid_from,
				NULL::DATE AS valid_to,
				NULL AS plant_id,
				TRUE AS is_approved,
				CURRENT_TIMESTAMP AS created_at
			FROM query_table(items_table)
		),
		bom_components AS (
			SELECT
				'COMP_' || parent_id || '_' || ROW_NUMBER() OVER (PARTITION BY parent_id ORDER BY child_id) AS component_id,
				'BOM_' || parent_id AS bom_id,
				ROW_NUMBER() OVER (PARTITION BY parent_id ORDER BY child_id)::INTEGER AS line_number,
				child_id AS child_material_id,
				COALESCE(quantity, 1) AS quantity_per,
				'EA' AS quantity_uom,
				FALSE AS is_fixed_quantity,
				0 AS scrap_percent,
				NULL::DATE AS effective_from,
				NULL::DATE AS effective_to,
				'STOCK' AS component_type,
				'PUSH' AS supply_type,
				NULL::INTEGER AS operation_sequence,
				FALSE AS is_alternative,
				NULL AS alternative_group,
				CURRENT_TIMESTAMP AS created_at
			FROM query_table(items_table)
		)
		SELECT 'materials' AS table_type,
			material_id::VARCHAR, material_id AS material_number, '' AS description,
			NULL::VARCHAR AS material_type, NULL::VARCHAR AS material_group, NULL::VARCHAR AS procurement_type,
			'EA' AS base_uom, NULL::VARCHAR AS weight, NULL::VARCHAR AS cost_per_unit,
			source_system::VARCHAR, TRUE::VARCHAR AS is_active, CURRENT_TIMESTAMP::VARCHAR AS created_at,
			NULL::VARCHAR AS col13, NULL::VARCHAR AS col14, NULL::VARCHAR AS col15,
			NULL::VARCHAR AS col16
		FROM (
			SELECT material_id, source_system FROM unique_materials
			CROSS JOIN (SELECT source_system FROM bom_headers LIMIT 1)
		)
		UNION ALL
		SELECT 'bom_header' AS table_type,
			bom_id::VARCHAR, source_bom_id::VARCHAR, parent_material_id::VARCHAR, bom_type::VARCHAR,
			alternative_number::VARCHAR, revision::VARCHAR, base_quantity::VARCHAR, base_uom::VARCHAR,
			valid_from::VARCHAR, valid_to::VARCHAR, plant_id::VARCHAR, is_approved::VARCHAR, created_at::VARCHAR,
			NULL::VARCHAR, NULL::VARCHAR, NULL::VARCHAR
		FROM bom_headers
		UNION ALL
		SELECT 'bom_component' AS table_type,
			component_id::VARCHAR, bom_id::VARCHAR, line_number::VARCHAR, child_material_id::VARCHAR,
			quantity_per::VARCHAR, quantity_uom::VARCHAR, is_fixed_quantity::VARCHAR, scrap_percent::VARCHAR,
			effective_from::VARCHAR, effective_to::VARCHAR, component_type::VARCHAR, supply_type::VARCHAR,
			operation_sequence::VARCHAR, is_alternative::VARCHAR, alternative_group::VARCHAR, created_at::VARCHAR
		FROM bom_components
	)");
	CheckQueryResult(result, "create items_to_bom macro");
}

} // namespace anofox
} // namespace duckdb
