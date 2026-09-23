#include "modules/dynamics365_transformations.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"
#include "datazoo_function_doc.hpp"

namespace ddoc = datazoo::doc;

namespace duckdb {
namespace anofox {

static const DefaultTableMacro DYNAMICS365_TO_MATERIALS_MACRO = {
    DEFAULT_SCHEMA,
    "dynamics365_to_materials",
    {nullptr},
    {{"invent_table", "'InventTable'"}, {nullptr, nullptr}},
    R"(
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
		FROM query_table(invent_table))"};

static const DefaultTableMacro DYNAMICS365_TO_BOM_HEADER_MACRO = {
    DEFAULT_SCHEMA,
    "dynamics365_to_bom_header",
    {nullptr},
    {{"bom_table", "'BOMTable'"}, {"bom_version", "'BOMVersion'"}, {nullptr, nullptr}},
    R"(
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
		WHERE bt.Status = 0)"};

static const DefaultTableMacro DYNAMICS365_TO_BOM_COMPONENT_MACRO = {
    DEFAULT_SCHEMA,
    "dynamics365_to_bom_component",
    {nullptr},
    {{"bom_lines", "'BOM'"}, {nullptr, nullptr}},
    R"(
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
		FROM query_table(bom_lines))"};

void RegisterDynamics365TransformationMacros(ExtensionLoader &loader) {
	ddoc::Registrar reg(loader, {"similarity", "erp"});
	reg.RegisterTableMacro(DYNAMICS365_TO_MATERIALS_MACRO,
	           {ddoc::Doc()
	                .Describe("Maps a Dynamics 365 InventTable to the universal materials shape. Reads ItemId, ItemName and ItemType (0/1/2) and returns the full materials column set. Projects a shape; it does not write an output table.")
	                .Example("SELECT * FROM dynamics365_to_materials(invent_table := 'InventTable')")});
	reg.RegisterTableMacro(DYNAMICS365_TO_BOM_HEADER_MACRO,
	           {ddoc::Doc()
	                .Describe("Maps Dynamics 365 BOMTable joined to BOMVersion into the universal bom_header shape, keeping rows with Status = 0. Reads BOMTable.(BOMId, ItemId, Status) and BOMVersion.(BOMId, VersionId, ActivationDate, ApprovedStatus).")
	                .Example("SELECT * FROM dynamics365_to_bom_header(bom_table := 'BOMTable', bom_version := 'BOMVersion')")});
	reg.RegisterTableMacro(DYNAMICS365_TO_BOM_COMPONENT_MACRO,
	           {ddoc::Doc()
	                .Describe("Maps Dynamics 365 BOM lines into the universal bom_component shape (singular) \u2014 note this is not the flat bom_items form. Reads BOMId, LineNum, ItemId, Quantity and ScrapPercent.")
	                .Example("SELECT * FROM dynamics365_to_bom_component(bom_lines := 'BOM')")});
}

} // namespace anofox
} // namespace duckdb
