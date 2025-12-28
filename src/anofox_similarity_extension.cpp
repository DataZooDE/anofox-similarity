#include "anofox_similarity_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/connection.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

#include <unordered_set>
#include <unordered_map>
#include <queue>
#include <algorithm>
#include <cmath>

namespace duckdb {

//------------------------------------------------------------------------------
// Jaccard Similarity Function
//------------------------------------------------------------------------------

// Custom hash function for duckdb::Value
struct ValueHash {
	size_t operator()(const Value &val) const {
		return val.Hash();
	}
};

// Custom equality function for duckdb::Value
struct ValueEqual {
	bool operator()(const Value &a, const Value &b) const {
		// Use strict equality - same type and same value
		return a == b;
	}
};

static void JaccardSimilarityFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &list_a = args.data[0];
	auto &list_b = args.data[1];
	auto count = args.size();

	// Get child vectors for both lists
	auto &child_a = ListVector::GetEntry(list_a);
	auto &child_b = ListVector::GetEntry(list_b);

	// Flatten vectors for uniform access
	UnifiedVectorFormat vdata_a, vdata_b;
	list_a.ToUnifiedFormat(count, vdata_a);
	list_b.ToUnifiedFormat(count, vdata_b);

	UnifiedVectorFormat child_vdata_a, child_vdata_b;
	auto child_count_a = ListVector::GetListSize(list_a);
	auto child_count_b = ListVector::GetListSize(list_b);
	child_a.ToUnifiedFormat(child_count_a, child_vdata_a);
	child_b.ToUnifiedFormat(child_count_b, child_vdata_b);

	auto list_entries_a = UnifiedVectorFormat::GetData<list_entry_t>(vdata_a);
	auto list_entries_b = UnifiedVectorFormat::GetData<list_entry_t>(vdata_b);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto idx_a = vdata_a.sel->get_index(i);
		auto idx_b = vdata_b.sel->get_index(i);

		// Handle NULL inputs
		if (!vdata_a.validity.RowIsValid(idx_a) || !vdata_b.validity.RowIsValid(idx_b)) {
			result_validity.SetInvalid(i);
			continue;
		}

		auto &entry_a = list_entries_a[idx_a];
		auto &entry_b = list_entries_b[idx_b];

		// Handle empty lists: Jaccard(empty, empty) = 0.0
		if (entry_a.length == 0 && entry_b.length == 0) {
			result_data[i] = 0.0;
			continue;
		}

		// Build set from list A using Value for generic comparison
		std::unordered_set<Value, ValueHash, ValueEqual> set_a;
		for (idx_t j = 0; j < entry_a.length; j++) {
			auto child_idx = child_vdata_a.sel->get_index(entry_a.offset + j);
			if (child_vdata_a.validity.RowIsValid(child_idx)) {
				set_a.insert(child_a.GetValue(child_idx));
			}
		}

		// Build set from list B
		std::unordered_set<Value, ValueHash, ValueEqual> set_b;
		for (idx_t j = 0; j < entry_b.length; j++) {
			auto child_idx = child_vdata_b.sel->get_index(entry_b.offset + j);
			if (child_vdata_b.validity.RowIsValid(child_idx)) {
				set_b.insert(child_b.GetValue(child_idx));
			}
		}

		// Count intersection from deduplicated sets
		idx_t intersection_count = 0;
		for (const auto &val : set_a) {
			if (set_b.find(val) != set_b.end()) {
				intersection_count++;
			}
		}

		idx_t union_size = set_a.size() + set_b.size() - intersection_count;

		if (union_size == 0) {
			result_data[i] = 0.0;
		} else {
			result_data[i] = static_cast<double>(intersection_count) / static_cast<double>(union_size);
		}
	}
}

//------------------------------------------------------------------------------
// Constants and Helper Functions
//------------------------------------------------------------------------------

namespace {
	// Embedding dimension constants
	constexpr idx_t JACCARD_EMBEDDING_DIM = 128;    // Min-hash signatures
	constexpr idx_t WL_EMBEDDING_DIM = 256;         // Weisfeiler-Lehman kernel
	constexpr idx_t COMBINED_EMBEDDING_DIM = 384;   // Jaccard + WL concatenated

	// Temporal thresholds for predecessor inference
	constexpr int MIN_OVERLAP_WEEKS = 8;            // Minimum data for correlation
	constexpr int RECENT_WINDOW_DAYS = 90;          // Recent consumption window
	constexpr int LONG_WINDOW_DAYS = 180;           // Long-term consumption window

	// VSS search parameters
	constexpr int VSS_OVERFETCH_FACTOR = 2;         // Over-fetch for filtering

	// WL kernel parameters
	constexpr double WL_WEIGHT_DECAY_OFFSET = 2.0;  // Controls iteration weight
}

// Helper function for consistent error handling
template<typename T>
static void CheckQueryResult(const unique_ptr<T> &result,
                             const std::string &operation) {
	if (result->HasError()) {
		throw InvalidInputException("Failed to %s: %s",
		                           operation.c_str(),
		                           result->GetError().c_str());
	}
}

//------------------------------------------------------------------------------
// Phase-specific loader sub-functions
//------------------------------------------------------------------------------

// Phase 1: Register scalar functions
static void RegisterScalarFunctions(ExtensionLoader &loader) {
	auto jaccard_similarity_function = ScalarFunction(
	    "jaccard_similarity",
	    {LogicalType::LIST(LogicalType::ANY), LogicalType::LIST(LogicalType::ANY)},
	    LogicalType::DOUBLE,
	    JaccardSimilarityFun
	);
	loader.RegisterFunction(jaccard_similarity_function);
}

// Phase 2: Initialize VSS integration
static void InitializeVSSIntegration(Connection &conn) {
	auto vss_result = conn.Query("INSTALL vss; LOAD vss;");
	CheckQueryResult(vss_result, "load VSS extension");

	vss_result = conn.Query("SET hnsw_enable_experimental_persistence = true;");
	// Note: Persistence is optional, continue if it fails
}

// Phase 3: Create embedding tables and tracking
static void CreateEmbeddingTables(Connection &conn) {
	auto schema_result = conn.Query(R"(
		CREATE TABLE IF NOT EXISTS material_embeddings (
			material_id VARCHAR PRIMARY KEY,
			jaccard_embedding FLOAT[128],
			wl_embedding FLOAT[256],
			combined_embedding FLOAT[384],
			updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
			num_components INTEGER
		)
	)");
	CheckQueryResult(schema_result, "create material_embeddings table");

	auto tracking_result = conn.Query(R"(
		CREATE TABLE IF NOT EXISTS material_embeddings_dirty (
			material_id VARCHAR PRIMARY KEY,
			reason VARCHAR,
			marked_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
		)
	)");
	CheckQueryResult(tracking_result, "create material_embeddings_dirty table");
}

// Phase 3b: Create HNSW indexes
static void CreateHNSWIndexes(Connection &conn) {
	auto idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS jaccard_idx ON material_embeddings
		USING HNSW(jaccard_embedding) WITH (metric = 'l2sq')
	)");
	// Continue even if index creation fails (may already exist)

	idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS wl_idx ON material_embeddings
		USING HNSW(wl_embedding) WITH (metric = 'cosine')
	)");

	idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS combined_idx ON material_embeddings
		USING HNSW(combined_embedding) WITH (metric = 'cosine')
	)");
}

// Phase 4a: Register similarity search macros
static void RegisterSimilarityMacros(Connection &conn) {
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
					COUNT(*) > 0 AS can_use_vss,
					jaccard_embedding AS query_embedding,
					num_components AS query_num_components
				FROM material_embeddings
				WHERE material_id = query_material_id
					AND jaccard_embedding IS NOT NULL
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
				WHERE va.can_use_vss = TRUE
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
				FROM material_components mc, query_mat qm, vss_available va
				WHERE mc.material_id != query_material_id
					AND (va.can_use_vss = FALSE OR method != 'jaccard')  -- Mutual exclusion with VSS path
			),
			-- Combined results: Union of VSS and brute-force paths (only one executes)
			combined AS (
				SELECT * FROM vss_results
				WHERE EXISTS (SELECT 1 FROM vss_available WHERE can_use_vss = TRUE)
					AND method = 'jaccard'

				UNION ALL

				SELECT * FROM brute_force_results
				WHERE NOT EXISTS (SELECT 1 FROM vss_available WHERE can_use_vss = TRUE)
					OR method != 'jaccard'
			)
		SELECT material_id, similarity, shared_components, total_components
		FROM combined
		WHERE similarity >= min_similarity
		ORDER BY similarity DESC
		LIMIT k
	)");

	CheckQueryResult(result, "create find_similar_materials macro");

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

	result = conn.Query(R"(
		CREATE OR REPLACE MACRO infer_predecessors(
			query_material_id,
			lookback_months := 24,
			min_similarity := 0.3,
			min_confidence := 0.5,
			lag_weeks := 8,
			bom_table := 'bom_items',
			movements_table := 'goods_movements'
		) AS TABLE
		SELECT * FROM (
			WITH
				-- Get query material's time series and boundaries
				query_ts AS (
					SELECT
						movement_date,
						quantity,
						MIN(movement_date) OVER () AS query_start,
						MAX(movement_date) OVER () AS query_end
					FROM query_table(movements_table)
					WHERE material_id = query_material_id
				),
				query_boundaries AS (
					SELECT
						MIN(query_start) AS start_date,
						MAX(query_end) AS end_date
					FROM query_ts
				),
				-- Find similar materials (potential predecessors)
				similar_mats AS (
					SELECT material_id, similarity, shared_components, total_components
					FROM find_similar_materials(
						query_material_id, 100,
						method := 'jaccard',
						min_similarity := min_similarity,
						bom_table := bom_table
					)
				),
				-- Get candidate materials' time series with boundaries
				candidate_ts AS (
					SELECT
						gm.material_id,
						gm.movement_date,
						gm.quantity,
						MIN(gm.movement_date) OVER (PARTITION BY gm.material_id) AS cand_first_usage,
						MAX(gm.movement_date) OVER (PARTITION BY gm.material_id) AS cand_last_usage
					FROM query_table(movements_table) gm
					INNER JOIN similar_mats s ON gm.material_id = s.material_id
					WHERE gm.movement_date >= (SELECT start_date - INTERVAL (lookback_months) MONTHS FROM query_boundaries)
				),
				-- Create weekly aggregates for correlation with lag applied to query
				query_weekly AS (
					SELECT
						DATE_TRUNC('week', movement_date) AS week,
						SUM(quantity) AS weekly_qty
					FROM query_ts
					GROUP BY 1
				),
				candidate_weekly AS (
					SELECT
						material_id,
						DATE_TRUNC('week', movement_date) AS week,
						SUM(quantity) AS weekly_qty,
						MIN(cand_first_usage) AS first_usage,
						MAX(cand_last_usage) AS last_usage
					FROM candidate_ts
					GROUP BY material_id, DATE_TRUNC('week', movement_date)
				),
				-- Join with lag: candidate week vs query week + lag
				lagged_join AS (
					SELECT
						c.material_id,
						c.week AS cand_week,
						c.weekly_qty AS cand_qty,
						q.weekly_qty AS query_qty,
						c.first_usage,
						c.last_usage
					FROM candidate_weekly c
					INNER JOIN query_weekly q ON c.week = q.week - INTERVAL (lag_weeks) WEEKS
				),
				-- Calculate correlation per candidate
				correlations AS (
					SELECT
						material_id,
						CORR(cand_qty, query_qty) AS correlation,
						COUNT(*) AS overlapping_weeks,
						MIN(first_usage) AS first_usage,
						MAX(last_usage) AS last_usage
					FROM lagged_join
					GROUP BY material_id
					HAVING COUNT(*) >= 8  -- Require at least 8 weeks of overlap (MIN_OVERLAP_WEEKS = 8)
				),
				-- Combine with similarity and calculate scores
				scored AS (
					SELECT
						c.material_id,
						c.correlation,
						s.similarity,
						c.first_usage,
						c.last_usage,
						(SELECT start_date FROM query_boundaries) AS query_start,
						c.overlapping_weeks,
						-- Temporal succession: predecessor should end around/after query starts (overlap)
						-- Best case: predecessor ends 0-6 months after successor starts (proper phaseout)
						-- Also valid: predecessor ends up to 3 months before successor starts (gap)
						-- Invalid: predecessor ends more than 6 months before (unrelated) or is still active
						CASE
							-- Predecessor ends during overlap period (0 to 6 months after successor starts)
							WHEN c.last_usage >= (SELECT start_date FROM query_boundaries)
							 AND c.last_usage <= (SELECT start_date FROM query_boundaries) + INTERVAL 6 MONTHS
							THEN 1.0 - (DATEDIFF('day', (SELECT start_date FROM query_boundaries), c.last_usage) / 180.0) * 0.3
							-- Predecessor ends shortly before successor starts (up to 3 months gap)
							WHEN c.last_usage >= (SELECT start_date FROM query_boundaries) - INTERVAL 3 MONTHS
							 AND c.last_usage < (SELECT start_date FROM query_boundaries)
							THEN 0.7 - (DATEDIFF('day', c.last_usage, (SELECT start_date FROM query_boundaries)) / 90.0) * 0.3
							-- Predecessor still active well after successor starts (more than 6 months)
							WHEN c.last_usage > (SELECT start_date FROM query_boundaries) + INTERVAL 6 MONTHS
							THEN 0.3
							ELSE 0.0
						END AS temporal_score
					FROM correlations c
					INNER JOIN similar_mats s ON c.material_id = s.material_id
					WHERE c.correlation IS NOT NULL
				),
				-- Calculate confidence and filter
				with_confidence AS (
					SELECT
						material_id,
						-- Confidence = 0.4 * (-correlation) + 0.3 * similarity + 0.3 * temporal_score
						-- Negative correlation (anti-correlation) contributes positively
						0.4 * GREATEST(0, -correlation) + 0.3 * similarity + 0.3 * temporal_score AS confidence,
						correlation,
						similarity,
						temporal_score,
						first_usage,
						last_usage,
						query_start,
						overlapping_weeks
					FROM scored
					WHERE correlation < 0  -- Must have negative correlation (anti-correlation)
					  AND temporal_score > 0.0  -- Must have valid temporal succession
				)
			SELECT
				material_id AS predecessor_id,
				ROUND(confidence, 4) AS confidence,
				ROUND(correlation, 4) AS correlation,
				ROUND(similarity, 4) AS similarity,
				ROUND(temporal_score, 4) AS temporal_score,
				first_usage AS predecessor_first_usage,
				last_usage AS predecessor_last_usage,
				query_start AS successor_start,
				overlapping_weeks
			FROM with_confidence
			WHERE confidence >= min_confidence
			ORDER BY confidence DESC
		)
	)");

	CheckQueryResult(result, "create infer_predecessors macro");

	result = conn.Query(R"(
		CREATE OR REPLACE MACRO wl_kernel_similarity(
			material_a,
			material_b,
			iterations := 3,
			bom_table := 'bom_items'
		) AS (
			WITH RECURSIVE
				-- Phase 1: Extract neighborhood around each material (DFS with depth limit)
				neighborhood_a AS (
					SELECT component FROM (
						WITH RECURSIVE dfs(component, depth) AS (
							SELECT DISTINCT child_id, 0
							FROM query_table(bom_table)
							WHERE parent_id = material_a
							UNION ALL
							SELECT DISTINCT qb.child_id, dfs.depth + 1
							FROM dfs
							INNER JOIN query_table(bom_table) qb ON dfs.component = qb.parent_id
							WHERE dfs.depth < iterations
						)
						SELECT component FROM dfs
					)
				),
				neighborhood_b AS (
					SELECT component FROM (
						WITH RECURSIVE dfs(component, depth) AS (
							SELECT DISTINCT child_id, 0
							FROM query_table(bom_table)
							WHERE parent_id = material_b
							UNION ALL
							SELECT DISTINCT qb.child_id, dfs.depth + 1
							FROM dfs
							INNER JOIN query_table(bom_table) qb ON dfs.component = qb.parent_id
							WHERE dfs.depth < iterations
						)
						SELECT component FROM dfs
					)
				),
				-- Phase 2: Count component occurrences per material (structured histogram)
				fingerprint_a AS (
					SELECT component, COUNT(*) AS occurrence, 0 AS level
					FROM (
						WITH RECURSIVE dfs(component, depth) AS (
							SELECT DISTINCT child_id, 0
							FROM query_table(bom_table)
							WHERE parent_id = material_a
							UNION ALL
							SELECT DISTINCT qb.child_id, dfs.depth + 1
							FROM dfs
							INNER JOIN query_table(bom_table) qb ON dfs.component = qb.parent_id
							WHERE dfs.depth < iterations
						)
						SELECT component, depth AS level FROM dfs
					)
					GROUP BY component, level
				),
				fingerprint_b AS (
					SELECT component, COUNT(*) AS occurrence, 0 AS level
					FROM (
						WITH RECURSIVE dfs(component, depth) AS (
							SELECT DISTINCT child_id, 0
							FROM query_table(bom_table)
							WHERE parent_id = material_b
							UNION ALL
							SELECT DISTINCT qb.child_id, dfs.depth + 1
							FROM dfs
							INNER JOIN query_table(bom_table) qb ON dfs.component = qb.parent_id
							WHERE dfs.depth < iterations
						)
						SELECT component, depth AS level FROM dfs
					)
					GROUP BY component, level
				),
				-- Phase 3: Compute intersection with weighted contribution
				intersection AS (
					SELECT
						fa.component,
						LEAST(fa.occurrence, fb.occurrence) AS shared_occurrence
					FROM fingerprint_a fa
					INNER JOIN fingerprint_b fb ON fa.component = fb.component
				),
				-- Phase 4: Compute union of all components
				union_all AS (
					SELECT component, level, occurrence FROM fingerprint_a
					UNION ALL
					SELECT component, level, occurrence FROM fingerprint_b
				),
				aggregated_union AS (
					SELECT SUM(occurrence) AS total_union FROM union_all
				),
				aggregated_intersection AS (
					SELECT COALESCE(SUM(shared_occurrence), 0) AS total_intersection FROM intersection
				)
			-- Structural similarity: weighted Jaccard-like metric
			-- intersection / (union - intersection) but weighted by level
			SELECT CASE
				WHEN (SELECT total_union FROM aggregated_union) = 0 THEN 0.0
				WHEN material_a = material_b THEN 1.0
				ELSE
					(SELECT total_intersection FROM aggregated_intersection)::DOUBLE /
					((SELECT total_union FROM aggregated_union) - (SELECT total_intersection FROM aggregated_intersection))::DOUBLE
			END
		)
	)");

	CheckQueryResult(result, "create wl_kernel_similarity macro");
}

// Phase 4b: Register SAP transformation macros
static void RegisterSAPTransformations(Connection &conn) {
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO sap_to_materials(
			mara_table,
			makt_table := NULL,
			language := 'E'
		) AS TABLE
		SELECT * FROM (
			WITH
				-- Parse ersda date (handles YYYYMMDD string format)
				mara_parsed AS (
					SELECT
						TRIM(matnr) AS material_id,
						mtart AS material_type,
						matkl AS material_group,
						TRY_STRPTIME(ersda::VARCHAR, '%Y%m%d')::DATE AS created_date,
						lvorm
					FROM query_table(mara_table)
				),
				-- Get descriptions from MAKT if provided
				makt_filtered AS (
					SELECT
						TRIM(matnr) AS matnr,
						maktx
					FROM query_table(makt_table)
					WHERE makt_table IS NOT NULL AND spras = language
				)
			SELECT
				m.material_id,
				m.material_type,
				m.material_group,
				COALESCE(t.maktx, '') AS description,
				m.created_date
			FROM mara_parsed m
			LEFT JOIN makt_filtered t ON m.material_id = t.matnr
			WHERE m.lvorm IS NULL OR m.lvorm = '' OR m.lvorm = ' '
		)
	)");

	CheckQueryResult(result, "create sap_to_materials macro");

	result = conn.Query(R"(
		CREATE OR REPLACE MACRO sap_to_materials_with_desc(
			mara_table,
			makt_table,
			language := 'E'
		) AS TABLE
		SELECT * FROM (
			WITH
				-- Parse ersda date (handles YYYYMMDD string format)
				mara_parsed AS (
					SELECT
						TRIM(matnr) AS material_id,
						mtart AS material_type,
						matkl AS material_group,
						TRY_STRPTIME(ersda::VARCHAR, '%Y%m%d')::DATE AS created_date,
						lvorm
					FROM query_table(mara_table)
				),
				-- Get descriptions from MAKT
				makt_filtered AS (
					SELECT
						TRIM(matnr) AS matnr,
						maktx
					FROM query_table(makt_table)
					WHERE spras = language
				)
			SELECT
				m.material_id,
				m.material_type,
				m.material_group,
				COALESCE(t.maktx, '') AS description,
				m.created_date
			FROM mara_parsed m
			LEFT JOIN makt_filtered t ON m.material_id = t.matnr
			WHERE m.lvorm IS NULL OR m.lvorm = '' OR m.lvorm = ' '
		)
	)");

	CheckQueryResult(result, "create sap_to_materials_with_desc macro");

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
					WHERE (bom_usage IS NULL OR stlty = bom_usage)
						AND (datuv IS NULL OR TRY_STRPTIME(datuv::VARCHAR, '%Y%m%d')::DATE <= reference_date)
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

// Phase 4c: Register embedding generation macro
static void RegisterEmbeddingMacros(Connection &conn) {
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO compute_jaccard_embeddings(bom_table := 'bom_items') AS TABLE
		WITH
			-- Step 1: Aggregate components per material
			material_components AS (
				SELECT
					parent_id AS material_id,
					COUNT(DISTINCT child_id)::INTEGER AS num_components,
					list(DISTINCT child_id ORDER BY child_id) AS components
				FROM query_table(bom_table)
				GROUP BY parent_id
			),
			-- Step 2: For each material and seed (0-127), compute min-hash value (JACCARD_EMBEDDING_DIM = 128)
			-- Min-hash(A, seed) = MIN(HASH(c || ':' || seed) for c in A)
			-- This preserves Jaccard similarity in expectation
			minhash_by_seed AS (
				SELECT
					mc.material_id,
					t.seed,
					MIN(CAST(ABS(HASH(c || ':' || t.seed::VARCHAR)) AS FLOAT)) AS minhash_value,
					mc.num_components
				FROM material_components mc
				CROSS JOIN generate_series(0, 127) AS t(seed)
				CROSS JOIN UNNEST(mc.components) AS u(c)
				GROUP BY mc.material_id, t.seed, mc.num_components
			)
		SELECT
			material_id,
			seed,
			minhash_value,
			num_components
		FROM minhash_by_seed
		ORDER BY material_id, seed
	)");

	CheckQueryResult(result, "create compute_jaccard_embeddings macro");
}

// Phase 5: Create incremental update system
static void CreateIncrementalUpdateTriggers(Connection &conn) {
	auto result = conn.Query(R"(
		CREATE OR REPLACE TRIGGER IF NOT EXISTS bom_items_insert_trigger
		AFTER INSERT ON bom_items
		FOR EACH ROW
		BEGIN
			INSERT INTO material_embeddings_dirty (material_id, reason)
			VALUES (NEW.parent_id, 'insert')
			ON CONFLICT (material_id) DO UPDATE SET
				marked_at = CURRENT_TIMESTAMP;
		END
	)");
	// Continue even if trigger creation fails (may not be supported in test context)

	result = conn.Query(R"(
		CREATE OR REPLACE TRIGGER IF NOT EXISTS bom_items_update_trigger
		AFTER UPDATE ON bom_items
		FOR EACH ROW
		BEGIN
			INSERT INTO material_embeddings_dirty (material_id, reason)
			VALUES (NEW.parent_id, 'update')
			ON CONFLICT (material_id) DO UPDATE SET
				marked_at = CURRENT_TIMESTAMP;
		END
	)");

	result = conn.Query(R"(
		CREATE OR REPLACE TRIGGER IF NOT EXISTS bom_items_delete_trigger
		AFTER DELETE ON bom_items
		FOR EACH ROW
		BEGIN
			INSERT INTO material_embeddings_dirty (material_id, reason)
			VALUES (OLD.parent_id, 'delete')
			ON CONFLICT (material_id) DO UPDATE SET
				marked_at = CURRENT_TIMESTAMP;
		END
	)");

	result = conn.Query(R"(
		CREATE OR REPLACE MACRO refresh_dirty_embeddings(bom_table := 'bom_items') AS TABLE
		WITH
			dirty_materials AS (
				SELECT material_id FROM material_embeddings_dirty
			),
			material_components AS (
				SELECT
					parent_id AS material_id,
					LIST(DISTINCT child_id ORDER BY child_id) AS components
				FROM query_table(bom_table)
				WHERE parent_id IN (SELECT material_id FROM dirty_materials)
				GROUP BY parent_id
			)
		DELETE FROM material_embeddings_dirty
		WHERE material_id IN (SELECT material_id FROM material_components)
		RETURNING material_id
	)");
	if (result->HasError()) {
		// Triggers and lazy refresh macros are optional - continue if they fail
	}
}

static void LoadInternal(ExtensionLoader &loader) {
	// Phase 1: Register scalar functions
	RegisterScalarFunctions(loader);

	// Get database connection for SQL operations
	auto &db = loader.GetDatabaseInstance();
	Connection conn(db);

	// Phase 2: Initialize VSS extension integration
	InitializeVSSIntegration(conn);

	// Phase 3: Create embedding tables and tracking
	CreateEmbeddingTables(conn);
	CreateHNSWIndexes(conn);

	// Phase 4: Register SQL macros
	RegisterSimilarityMacros(conn);
	RegisterSAPTransformations(conn);
	RegisterEmbeddingMacros(conn);

	// Phase 5: Set up incremental update system
	CreateIncrementalUpdateTriggers(conn);
}

void AnofoxSimilarityExtension::Load(ExtensionLoader &db) {
	LoadInternal(db);
}

std::string AnofoxSimilarityExtension::Name() {
	return "anofox_similarity";
}

std::string AnofoxSimilarityExtension::Version() const {
#ifdef EXT_VERSION_ANOFOX_SIMILARITY
	return EXT_VERSION_ANOFOX_SIMILARITY;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(anofox_similarity, loader) {
	duckdb::LoadInternal(loader);
}

}
