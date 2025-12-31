#include "modules/dynamics365_transformations.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

void RegisterDynamics365TransformationMacros(Connection &conn) {
	// Macro: dynamics365_to_materials - Convert D365 InventTable to universal materials
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO dynamics365_to_materials(
			invent_table := 'InventTable'
		) AS TABLE
		SELECT
			ItemId AS material_id,
			ItemId AS material_number,
			ItemName AS description,
			CASE ItemType
				WHEN 0 THEN 'ASSEMBLY'
				WHEN 1 THEN 'FINISHED'
				WHEN 2 THEN 'COMPONENT'
				ELSE 'OTHER'
			END AS material_type,
			NULL AS material_group,
			NULL AS procurement_type,
			'EA' AS base_uom,
			NULL AS weight,
			NULL AS cost_per_unit,
			'DYNAMICS365' AS source_system,
			TRUE AS is_active,
			CURRENT_TIMESTAMP AS created_at
		FROM query_table(invent_table)
	)");
	CheckQueryResult(result, "create dynamics365_to_materials macro");

	// Macro: dynamics365_to_bom_header - Convert D365 BOMTable/BOMVersion to universal bom_header
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO dynamics365_to_bom_header(
			bom_table := 'BOMTable',
			bom_version := 'BOMVersion'
		) AS TABLE
		SELECT
			bt.BOMId AS bom_id,
			'DYNAMICS365' AS source_system,
			bt.BOMId AS source_bom_id,
			bt.ItemId AS parent_material_id,
			'MANUFACTURING' AS bom_type,
			'01' AS alternative_number,
			bv.VersionId AS revision,
			1 AS base_quantity,
			'EA' AS base_uom,
			CAST(bv.ActivationDate AS DATE) AS valid_from,
			NULL::DATE AS valid_to,
			NULL AS plant_id,
			CASE WHEN bv.ApprovedStatus = 1 THEN TRUE ELSE FALSE END AS is_approved,
			CURRENT_TIMESTAMP AS created_at
		FROM query_table(bom_table) bt
		LEFT JOIN query_table(bom_version) bv ON bt.BOMId = bv.BOMId
		WHERE bt.Status = 0
	)");
	CheckQueryResult(result, "create dynamics365_to_bom_header macro");

	// Macro: dynamics365_to_bom_component - Convert D365 BOM to universal bom_component
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO dynamics365_to_bom_component(
			bom_lines := 'BOM'
		) AS TABLE
		SELECT
			'COMP_' || BOMId || '_' || LineNum::VARCHAR AS component_id,
			BOMId AS bom_id,
			LineNum AS line_number,
			ItemId AS child_material_id,
			Quantity AS quantity_per,
			'EA' AS quantity_uom,
			FALSE AS is_fixed_quantity,
			COALESCE(ScrapPercent, 0) AS scrap_percent,
			NULL::DATE AS effective_from,
			NULL::DATE AS effective_to,
			'STOCK' AS component_type,
			'PUSH' AS supply_type,
			LineNum AS operation_sequence,
			FALSE AS is_alternative,
			NULL AS alternative_group,
			CURRENT_TIMESTAMP AS created_at
		FROM query_table(bom_lines)
	)");
	CheckQueryResult(result, "create dynamics365_to_bom_component macro");
}

} // namespace anofox
} // namespace duckdb
