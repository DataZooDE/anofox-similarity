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
			SELECT TRIM(matnr) AS material_id, maktx
			FROM query_table(makt_table)
			WHERE spras = language
		) k ON m.material_id = k.material_id
	)");

	CheckQueryResult(result, "create sap_to_materials_with_desc macro");

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
				-- Step 1: Get current BOM header (highest version by DATUV)
				mast_parsed AS (
					SELECT
						TRIM(matnr) AS parent_id,
						stlnr AS bom_id,
						stlal AS alternative,
						ROW_NUMBER() OVER (PARTITION BY TRIM(matnr), stlal ORDER BY datuv DESC) AS rn
					FROM query_table(mast_table)
					WHERE (datuv IS NULL OR TRY_STRPTIME(datuv::VARCHAR, '%Y%m%d')::DATE <= reference_date)
				),
				-- Step 2: Get BOM structure (components per BOM)
				stko_parsed AS (
					SELECT
						stlnr AS bom_id,
						TRIM(posnr) AS line_num,
						TRIM(idnrk) AS component_id,
						menge AS qty,
						meins AS unit
					FROM query_table(stko_table)
				),
				-- Step 3: Get item validity (explicit versioning)
				stpo_parsed AS (
					SELECT
						stlnr AS bom_id,
						TRIM(posnr) AS line_num,
						TRY_STRPTIME(datuv::VARCHAR, '%Y%m%d')::DATE AS valid_from,
						LEAD(TRY_STRPTIME(datuv::VARCHAR, '%Y%m%d')::DATE)
							OVER (PARTITION BY stlnr, TRIM(posnr) ORDER BY datuv) AS valid_to
					FROM query_table(stpo_table)
				),
				-- Step 4: Join with versioning
				bom_joined AS (
					SELECT
						mp.parent_id,
						sk.component_id AS child_id,
						sk.qty,
						sk.unit,
						COALESCE(sp.valid_from, TRY_STRPTIME('19000101'::VARCHAR, '%Y%m%d')::DATE) AS valid_from,
						COALESCE(sp.valid_to, '9999-12-31'::DATE) AS valid_to,
						mp.alternative AS bom_version
					FROM mast_parsed mp
					INNER JOIN stko_parsed sk ON mp.bom_id = sk.bom_id
					LEFT JOIN stpo_parsed sp ON mp.bom_id = sp.bom_id AND sk.line_num = sp.line_num
					WHERE mp.rn = 1
						AND mp.alternative = bom_alternative
						AND reference_date BETWEEN COALESCE(sp.valid_from, '1900-01-01'::DATE)
						AND COALESCE(sp.valid_to, '9999-12-31'::DATE)
				)
			SELECT
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
