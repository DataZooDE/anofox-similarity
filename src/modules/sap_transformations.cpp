#include "modules/sap_transformations.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"
#include "datazoo_function_doc.hpp"

namespace ddoc = datazoo::doc;

namespace duckdb {
namespace anofox {

static const DefaultTableMacro SAP_TO_MATERIALS_MACRO = {
    DEFAULT_SCHEMA,
    "sap_to_materials",
    {"mara_table", nullptr},
    {{"makt_table", "NULL"}, {"language", "'E'"}, {nullptr, nullptr}},
    R"(
		SELECT
			TRIM(matnr) AS material_id,
			mtart AS material_type,
			matkl AS material_group,
			'' AS description,
			TRY_STRPTIME(ersda::VARCHAR, '%Y%m%d')::DATE AS created_date
		FROM query_table(mara_table)
		WHERE lvorm IS NULL OR lvorm = '' OR lvorm = ' ')"};

static const DefaultTableMacro SAP_TO_MATERIALS_WITH_DESC_MACRO = {
    DEFAULT_SCHEMA,
    "sap_to_materials_with_desc",
    {"mara_table", "makt_table", nullptr},
    {{"language", "'E'"}, {nullptr, nullptr}},
    R"(
		SELECT
			m.material_id,
			m.material_type,
			m.material_group,
			COALESCE(k.maktx, '') AS description,
			m.created_date
		FROM sap_to_materials(mara_table := mara_table) m
		LEFT JOIN (
			-- One description per material: real MAKT can have several rows per (matnr, spras)
			-- (e.g. across MANDT/client), which would otherwise fan the material out into duplicates.
			SELECT material_id, ANY_VALUE(maktx) AS maktx
			FROM (SELECT TRIM(matnr) AS material_id, maktx FROM query_table(makt_table) WHERE spras = COALESCE(language, 'E'))
			GROUP BY material_id
		) k ON m.material_id = k.material_id)"};

static const DefaultTableMacro EXTRACT_MATERIAL_DESCRIPTIONS_MACRO = {
    DEFAULT_SCHEMA,
    "extract_material_descriptions",
    {nullptr},
    {{"makt_table", "'sap_makt'"}, {"language", "'EN'"}, {nullptr, nullptr}},
    R"(
		WITH descriptions AS (
			-- Collapse multiple MAKT rows per (matnr, spras) to a single description so a material
			-- is not duplicated (real MAKT repeats per MANDT/client).
			SELECT
				TRIM(matnr) AS material_id,
				ANY_VALUE(TRIM(maktx)) AS description,
				ANY_VALUE(TRIM(maktg)) AS short_text
			FROM query_table(makt_table)
			WHERE spras = COALESCE(language, 'EN')
			GROUP BY TRIM(matnr)
		),
		combined_text AS (
			SELECT
				material_id,
				CONCAT_WS(' ', description, short_text) AS full_text
			FROM descriptions
			WHERE description IS NOT NULL OR short_text IS NOT NULL
		)
		SELECT * FROM combined_text)"};

static const DefaultTableMacro SAP_TO_BOM_ITEMS_MACRO = {
    DEFAULT_SCHEMA,
    "sap_to_bom_items",
    {"mast_table", "stko_table", "stpo_table", nullptr},
    {{"bom_alternative", "'01'"}, {"reference_date", "'9999-12-31'"}, {"bom_usage", "NULL"}, {nullptr, nullptr}},
    R"(
		SELECT * FROM (
			WITH
				-- Step 1: Get current BOM header with validity (JOIN MAST + STKO)
				mast_stko_parsed AS (
					SELECT
						TRIM(m.matnr) AS parent_id,
						s.stlnr AS bom_id,
						s.stlal AS alternative,
						s.datuv AS header_datuv,
						-- Partition by (matnr, stlnr, stlal): dedup only collapses validity SLICES of
						-- the SAME BOM number. Omitting stlnr previously dropped genuinely-distinct BOMs
						-- (e.g. plant-specific BOMs a material can carry under the same alternative).
						ROW_NUMBER() OVER (PARTITION BY TRIM(m.matnr), s.stlnr, s.stlal ORDER BY s.datuv DESC) AS rn
					FROM query_table(mast_table) m
					JOIN query_table(stko_table) s ON m.stlnr = s.stlnr
					-- SAP stores dates as YYYYMMDD strings; accept that AND ISO YYYY-MM-DD without
					-- a hard cast error (a plain CAST(... AS DATE) rejects '20240101').
					WHERE (s.datuv IS NULL
					       OR COALESCE(TRY_STRPTIME(s.datuv::VARCHAR, '%Y%m%d')::DATE, TRY_CAST(s.datuv AS DATE))
					          <= COALESCE(TRY_STRPTIME(COALESCE(reference_date, '9999-12-31')::VARCHAR, '%Y%m%d')::DATE,
					                      TRY_CAST(COALESCE(reference_date, '9999-12-31') AS DATE)))
				),
				-- Step 2: Get BOM components (FROM STPO, not STKO)
				stpo_components AS (
					SELECT
						stlnr AS bom_id,
						TRIM(stlkn) AS line_num,
						TRIM(idnrk) AS component_id,
						menge AS qty,
						meins AS unit
					FROM query_table(stpo_table)
				),
				-- Step 3: Join components with BOM header
				bom_joined AS (
					SELECT
						mp.bom_id,
						mp.parent_id,
						sc.component_id AS child_id,
						sc.qty,
						sc.unit,
						-- Derive validity from the selected header's datuv instead of a constant.
						COALESCE(TRY_STRPTIME(mp.header_datuv::VARCHAR, '%Y%m%d')::DATE,
						         TRY_CAST(mp.header_datuv AS DATE)) AS valid_from,
						'9999-12-31'::DATE AS valid_to,
						mp.alternative AS bom_version
					FROM mast_stko_parsed mp
					INNER JOIN stpo_components sc ON mp.bom_id = sc.bom_id
					WHERE mp.rn = 1
						AND mp.alternative = COALESCE(bom_alternative, '01')
				)
			SELECT
				bom_id,
				parent_id,
				child_id,
				qty,
				unit,
				valid_from,
				valid_to,
				bom_version
			FROM bom_joined
		))"};

void RegisterSAPTransformationMacros(ExtensionLoader &loader) {
	ddoc::Registrar reg(loader, {"similarity", "erp"});
	reg.RegisterTableMacro(SAP_TO_MATERIALS_MACRO,
	           {ddoc::Doc()
	                .Describe("Extracts materials from a SAP MARA table into the universal materials shape. Requires MARA columns matnr, mtart, matkl, ersda and lvorm; rows flagged deleted via lvorm are excluded. The description column is always empty here -- use sap_to_materials_with_desc to populate it from MAKT.")
	                .Example("SELECT * FROM sap_to_materials('MARA')")});
	reg.RegisterTableMacro(SAP_TO_MATERIALS_WITH_DESC_MACRO,
	           {ddoc::Doc()
	                .Describe("Like sap_to_materials, but joins MAKT to populate the description column in the requested 'language' (SAP single-character language key, e.g. 'E').")
	                .Example("SELECT * FROM sap_to_materials_with_desc('MARA', 'MAKT', language := 'E')")});
	reg.RegisterTableMacro(EXTRACT_MATERIAL_DESCRIPTIONS_MACRO,
	           {ddoc::Doc()
	                .Describe("Extracts material descriptions from a SAP MAKT table for one language, returning (material_id, description).")
	                .Example("SELECT * FROM extract_material_descriptions(makt_table := 'sap_makt', language := 'EN')")});
	reg.RegisterTableMacro(SAP_TO_BOM_ITEMS_MACRO,
	           {ddoc::Doc()
	                .Describe("Converts the SAP MAST/STKO/STPO trio into the universal BOM shape. 'bom_alternative' picks the alternative BOM, 'reference_date' selects the version valid on that date, and 'bom_usage' filters by usage (NULL means any). Quantity passes through the source MENGE column's type, which is DECIMAL in SAP -- cast if you need DOUBLE.")
	                .Example("SELECT * FROM sap_to_bom_items('MAST', 'STKO', 'STPO', bom_alternative := '01')")});
}

} // namespace anofox
} // namespace duckdb
