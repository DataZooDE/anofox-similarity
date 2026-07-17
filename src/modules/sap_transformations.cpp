#include "modules/sap_transformations.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

void RegisterSAPTransformationMacros(Connection &conn) {
	// sap_to_materials: Extract materials from MARA table
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO sap_to_materials(
			mara_table,
			makt_table := NULL,
			language := 'E'
		) AS TABLE
		SELECT
			TRIM(matnr) AS material_id,
			mtart AS material_type,
			matkl AS material_group,
			'' AS description,
			TRY_STRPTIME(ersda::VARCHAR, '%Y%m%d')::DATE AS created_date
		FROM query_table(mara_table)
		WHERE lvorm IS NULL OR lvorm = '' OR lvorm = ' '
	)");

	CheckQueryResult(result, "create sap_to_materials macro");

	// sap_to_materials_with_desc: Join MARA with MAKT for descriptions
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO sap_to_materials_with_desc(
			mara_table,
			makt_table,
			language := 'E'
		) AS TABLE
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
		) k ON m.material_id = k.material_id
	)");

	CheckQueryResult(result, "create sap_to_materials_with_desc macro");

	// extract_material_descriptions: Extract and combine material descriptions from MAKT
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO extract_material_descriptions(
			makt_table := 'sap_makt',
			language := 'EN'
		) AS TABLE
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
		SELECT * FROM combined_text
	)");

	CheckQueryResult(result, "create extract_material_descriptions macro");

	// sap_to_bom_items: Extract BOMs from MAST/STKO/STPO tables
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO sap_to_bom_items(
			mast_table,
			stko_table,
			stpo_table,
			bom_alternative := '01',
			reference_date := '9999-12-31',
			bom_usage := NULL
		) AS TABLE
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
		)
	)");

	CheckQueryResult(result, "create sap_to_bom_items macro");
}

} // namespace anofox
} // namespace duckdb
