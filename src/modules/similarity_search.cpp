#include "modules/similarity_search.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

void RegisterSimilaritySearchMacros(Connection &conn) {
	// find_similar_materials: Hybrid VSS/brute-force similarity search
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO find_similar_materials(
			query_material_id,
			k,
			method := 'jaccard',
			min_similarity := 0.0,
			bom_table := 'bom_items'
		) AS TABLE
		WITH
			-- Aggregate components per material
			material_components AS MATERIALIZED (
				SELECT parent_id AS material_id, list(child_id ORDER BY child_id) AS components
				FROM query_table(bom_table)
				GROUP BY parent_id
			),
			-- Get query material's components
			query_mat AS (
				SELECT components AS query_components
				FROM material_components
				WHERE material_id = query_material_id
			),
			-- VSS detection: Check if embeddings exist for query material
			vss_available AS (
				SELECT
					jaccard_embedding AS query_embedding,
					num_components AS query_num_components
				FROM material_embeddings
				WHERE material_id = query_material_id
					AND jaccard_embedding IS NOT NULL
				LIMIT 1
			),
			-- VSS k-NN search path: Fast approximate search using HNSW indexes
			vss_results AS (
				SELECT
					me.material_id,
					-- Convert L2 distance to Jaccard similarity approximation
					1.0 - (array_distance(me.jaccard_embedding, va.query_embedding) /
						   (array_distance(me.jaccard_embedding, va.query_embedding) + 1.0)) AS similarity,
					NULL::BIGINT AS shared_components,
					NULL::BIGINT AS total_components
				FROM material_embeddings me, vss_available va
				WHERE 1 = 1  -- vss_available provides embedding data
					AND method = 'jaccard'
					AND me.material_id != query_material_id
					AND me.jaccard_embedding IS NOT NULL
				-- HNSW index automatically triggers on this ORDER BY pattern
				ORDER BY array_distance(me.jaccard_embedding, va.query_embedding)
				LIMIT k * 2  -- Over-fetch for min_similarity filtering (VSS_OVERFETCH_FACTOR = 2)
			),
			-- Brute-force path: Exact computation for fallback or WL kernel method
			brute_force_results AS (
				SELECT
					mc.material_id,
					CASE
						WHEN method = 'wl_kernel' THEN
							wl_kernel_similarity(query_material_id, mc.material_id, bom_table := bom_table)
						ELSE
							jaccard_similarity(qm.query_components, mc.components)
					END AS similarity,
					len(list_intersect(qm.query_components, mc.components))::BIGINT AS shared_components,
					len(list_distinct(list_concat(qm.query_components, mc.components)))::BIGINT AS total_components
				FROM material_components mc, query_mat qm
				WHERE mc.material_id != query_material_id
					AND (NOT EXISTS (SELECT 1 FROM vss_available) OR method != 'jaccard')  -- Mutual exclusion with VSS path
			),
			-- Combined results: Union of VSS and brute-force paths (only one executes)
			combined AS (
				SELECT * FROM vss_results
				WHERE EXISTS (SELECT 1 FROM vss_available)
					AND method = 'jaccard'

				UNION ALL

				SELECT * FROM brute_force_results
				WHERE NOT EXISTS (SELECT 1 FROM vss_available)
					OR method != 'jaccard'
			)
		SELECT material_id, similarity, shared_components, total_components
		FROM combined
		WHERE similarity >= min_similarity
		ORDER BY similarity DESC
		LIMIT k
	)");

	CheckQueryResult(result, "create find_similar_materials macro");

	// cold_start_analogs: Historical consumption-aware analog discovery
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO cold_start_analogs(
			query_material_id,
			k,
			min_history_months := 0,
			min_similarity := 0.0,
			bom_table := 'bom_items',
			movements_table := 'goods_movements'
		) AS TABLE
		SELECT * FROM (
			WITH
				-- Aggregate components per material
				material_components AS MATERIALIZED (
					SELECT parent_id AS material_id, list(child_id ORDER BY child_id) AS components
					FROM query_table(bom_table)
					GROUP BY parent_id
				),
				-- Get query material's components
				query_mat AS (
					SELECT components AS query_components
					FROM material_components
					WHERE material_id = query_material_id
				),
				-- Compute similarities with all other materials
				computed_similarity AS (
					SELECT
						mc.material_id,
						jaccard_similarity(qm.query_components, mc.components) AS similarity,
						len(list_intersect(qm.query_components, mc.components))::BIGINT AS shared_components,
						len(list_distinct(list_concat(qm.query_components, mc.components)))::BIGINT AS total_components
					FROM material_components mc, query_mat qm
					WHERE mc.material_id != query_material_id
				),
				-- Calculate consumption history per material
				material_history AS (
					SELECT
						material_id,
						MIN(movement_date) AS first_usage,
						MAX(movement_date) AS last_usage,
						-- Calculate months between first and last usage
						(EXTRACT(YEAR FROM MAX(movement_date)) - EXTRACT(YEAR FROM MIN(movement_date))) * 12 +
						(EXTRACT(MONTH FROM MAX(movement_date)) - EXTRACT(MONTH FROM MIN(movement_date))) AS history_months
					FROM query_table(movements_table)
					GROUP BY material_id
				),
				-- Join similarity with history and filter
				with_history AS (
					SELECT
						cs.material_id,
						cs.similarity,
						cs.shared_components,
						cs.total_components,
						COALESCE(mh.history_months, 0)::BIGINT AS history_months,
						mh.first_usage,
						mh.last_usage
					FROM computed_similarity cs
					LEFT JOIN material_history mh ON cs.material_id = mh.material_id
					WHERE cs.similarity >= min_similarity
					  AND COALESCE(mh.history_months, 0) >= min_history_months
				)
			SELECT material_id, similarity, shared_components, total_components,
			       history_months, first_usage, last_usage
			FROM with_history
			ORDER BY similarity DESC
			LIMIT k
		)
	)");

	CheckQueryResult(result, "create cold_start_analogs macro");
}

} // namespace anofox
} // namespace duckdb
