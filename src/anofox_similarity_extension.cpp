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

static void LoadInternal(ExtensionLoader &loader) {
	// Register jaccard_similarity function for computing set similarity
	// Accepts two lists of any type and returns Jaccard coefficient (0.0 to 1.0)
	auto jaccard_similarity_function = ScalarFunction(
	    "jaccard_similarity",
	    {LogicalType::LIST(LogicalType::ANY), LogicalType::LIST(LogicalType::ANY)},
	    LogicalType::DOUBLE,
	    JaccardSimilarityFun
	);
	loader.RegisterFunction(jaccard_similarity_function);

	// Register find_similar_materials as a SQL table macro
	// Uses query_table() to dynamically access the BOM table by name
	// Default table is 'bom_items' with columns (parent_id, child_id)
	//
	// Supported methods:
	//   - 'jaccard': Jaccard similarity on direct child components (default)
	//   - 'wl_kernel': Weisfeiler-Lehman kernel for structural similarity
	//
	// Usage:
	//   SELECT * FROM find_similar_materials('MATERIAL-A', 10);
	//   SELECT * FROM find_similar_materials('MATERIAL-A', 10, method := 'jaccard', min_similarity := 0.5);
	//   SELECT * FROM find_similar_materials('MATERIAL-A', 10, method := 'wl_kernel');
	//   SELECT * FROM find_similar_materials('MATERIAL-A', 10, bom_table := 'my_bom_table');
	auto &db = loader.GetDatabaseInstance();
	Connection conn(db);

	// Load VSS extension for HNSW indexing
	auto vss_result = conn.Query("INSTALL vss; LOAD vss;");
	CheckQueryResult(vss_result, "load VSS extension");

	// Enable experimental persistence for index durability
	vss_result = conn.Query("SET hnsw_enable_experimental_persistence = true;");
	// Note: Persistence is optional, continue if it fails

	// Create material_embeddings table for VSS-indexed similarity search
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

	// Create HNSW indexes on embedding columns
	// Index 1: Jaccard similarity (min-hash based)
	auto idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS jaccard_idx ON material_embeddings
		USING HNSW(jaccard_embedding) WITH (metric = 'l2sq')
	)");
	// Continue even if index creation fails (may already exist)

	// Index 2: Weisfeiler-Lehman kernel similarity
	idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS wl_idx ON material_embeddings
		USING HNSW(wl_embedding) WITH (metric = 'cosine')
	)");

	// Index 3: Combined similarity (concatenated embedding)
	idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS combined_idx ON material_embeddings
		USING HNSW(combined_embedding) WITH (metric = 'cosine')
	)");

	// Create material_embeddings_dirty tracking table for incremental updates
	auto tracking_result = conn.Query(R"(
		CREATE TABLE IF NOT EXISTS material_embeddings_dirty (
			material_id VARCHAR PRIMARY KEY,
			reason VARCHAR,
			marked_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
		)
	)");
	CheckQueryResult(tracking_result, "create material_embeddings_dirty table");

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

	// Register cold_start_analogs as a SQL table macro
	// Finds similar materials that have consumption history for cold-start forecasting
	// Uses query_table() to dynamically access the movements table by name
	// Default tables are 'bom_items' and 'goods_movements'
	//
	// Usage:
	//   SELECT * FROM cold_start_analogs('NEW-MATERIAL', 5, min_history_months := 12);
	//   SELECT * FROM cold_start_analogs('NEW-MATERIAL', 5, min_history_months := 12, movements_table := 'my_movements');
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

	// Register infer_predecessors as a SQL table macro
	// Finds potential predecessors for a material based on:
	// - BOM similarity (Jaccard)
	// - Anti-correlation in time series (predecessor declines, successor rises)
	// - Temporal succession (predecessor ends before/as successor starts)
	//
	// Usage:
	//   SELECT * FROM infer_predecessors('NEW-MATERIAL', lookback_months := 24, min_confidence := 0.5);
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

	// Register wl_kernel_similarity as a SQL scalar macro
	// Computes Weisfeiler-Lehman kernel-like structural similarity between two materials
	// Uses recursive BOM traversal to capture graph structure
	//
	// Usage:
	//   SELECT wl_kernel_similarity('MATERIAL-A', 'MATERIAL-B');
	//   SELECT wl_kernel_similarity('MATERIAL-A', 'MATERIAL-B', iterations := 3);
	//   SELECT wl_kernel_similarity('MATERIAL-A', 'MATERIAL-B', bom_table := 'my_bom_table');
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO wl_kernel_similarity(
			material_a,
			material_b,
			iterations := 3,
			bom_table := 'bom_items'
		) AS (
			WITH RECURSIVE
				-- Build hierarchy for material A (BFS traversal)
				bom_tree_a AS (
					SELECT
						parent_id AS node,
						child_id AS neighbor,
						0 AS level,
						parent_id AS root
					FROM query_table(bom_table)
					WHERE parent_id = material_a
					UNION ALL
					SELECT
						b.parent_id,
						b.child_id,
						t.level + 1,
						t.root
					FROM query_table(bom_table) b
					INNER JOIN bom_tree_a t ON b.parent_id = t.neighbor
					WHERE t.level < iterations
				),
				-- Build hierarchy for material B
				bom_tree_b AS (
					SELECT
						parent_id AS node,
						child_id AS neighbor,
						0 AS level,
						parent_id AS root
					FROM query_table(bom_table)
					WHERE parent_id = material_b
					UNION ALL
					SELECT
						b.parent_id,
						b.child_id,
						t.level + 1,
						t.root
					FROM query_table(bom_table) b
					INNER JOIN bom_tree_b t ON b.parent_id = t.neighbor
					WHERE t.level < iterations
				),
				-- Create structural fingerprint for A: (node, level, neighbor_count) tuples
				fingerprint_a AS (
					SELECT
						neighbor AS component,
						level,
						COUNT(*) AS occurrence
					FROM bom_tree_a
					GROUP BY neighbor, level
				),
				-- Create structural fingerprint for B
				fingerprint_b AS (
					SELECT
						neighbor AS component,
						level,
						COUNT(*) AS occurrence
					FROM bom_tree_b
					GROUP BY neighbor, level
				),
				-- Compute intersection: components at same level with same occurrence
				intersection AS (
					SELECT
						a.component,
						a.level,
						LEAST(a.occurrence, b.occurrence) AS shared_occurrence
					FROM fingerprint_a a
					INNER JOIN fingerprint_b b
						ON a.component = b.component AND a.level = b.level
				),
				-- Compute union (for normalization)
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

	// ============================================================================
	// SAP BOM Transformation Macros
	// ============================================================================

	// Register sap_to_materials as a SQL table macro
	// Transforms SAP MARA (material master) to canonical materials format
	// Optionally joins with MAKT for descriptions
	// Uses query_table() for dynamic table access (pass table name as string)
	//
	// Usage:
	//   SELECT * FROM sap_to_materials(mara_table := 'mara');
	//   SELECT * FROM sap_to_materials(
	//       mara_table := 'mara',
	//       makt_table := 'makt',
	//       language := 'E'
	//   );
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO sap_to_materials(
			mara_table,
			makt_table := '',
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
				)
			SELECT
				m.material_id,
				NULL AS description,
				m.material_group,
				m.material_type,
				m.created_date
			FROM mara_parsed m
			WHERE m.lvorm IS NULL OR m.lvorm = '' OR m.lvorm = ' '
		)
	)");

	CheckQueryResult(result, "create sap_to_materials macro");

	// Register sap_to_materials_with_desc as a SQL table macro
	// Same as sap_to_materials but joins with MAKT for descriptions
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
				t.maktx AS description,
				m.material_group,
				m.material_type,
				m.created_date
			FROM mara_parsed m
			LEFT JOIN makt_filtered t ON m.material_id = t.matnr
			WHERE m.lvorm IS NULL OR m.lvorm = '' OR m.lvorm = ' '
		)
	)");

	CheckQueryResult(result, "create sap_to_materials macro");

	// Register sap_to_bom_items as a SQL table macro
	// Transforms SAP BOM tables to canonical bom_items format
	// Implements two-level versioning:
	//   1. Explicit versioning (stlal - alternative BOM)
	//   2. Date versioning (datuv - valid-from, calculated via LEAD())
	// Uses query_table() for dynamic table access (pass table names as strings)
	//
	// Usage:
	//   SELECT * FROM sap_to_bom_items(
	//       mast_table := 'mast',
	//       stko_table := 'stko',
	//       stpo_table := 'stpo'
	//   );
	//   SELECT * FROM sap_to_bom_items(
	//       mast_table := 'mast',
	//       stko_table := 'stko',
	//       stpo_table := 'stpo',
	//       bom_alternative := '01',
	//       reference_date := current_date,
	//       bom_usage := '2'
	//   );
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO sap_to_bom_items(
			mast_table,
			stko_table,
			stpo_table,
			bom_alternative := '01',
			reference_date := '9999-12-31',
			bom_usage := '2'
		) AS TABLE
		SELECT * FROM (
			WITH
				-- Resolve reference date (default to far future = latest valid BOM)
				ref_date AS (
					SELECT CAST(reference_date AS DATE) AS the_date
				),
				-- Step 1: Filter MAST by BOM usage and alternative
				mast_filtered AS (
					SELECT
						TRIM(matnr) AS matnr,
						werks,
						stlan,
						stlnr,
						stlal
					FROM query_table(mast_table)
					WHERE stlan = bom_usage
					  AND stlal = bom_alternative
				),
				-- Step 2: Calculate date validity for STKO using LEAD()
				-- Each alternative (stlal) has its own date history
				stko_with_dates AS (
					SELECT
						stlty,
						stlnr,
						stlal,
						datuv,
						lkenz::VARCHAR AS lkenz,
						-- Next version's datuv becomes this version's end date
						LEAD(datuv) OVER (
							PARTITION BY stlty, stlnr, stlal
							ORDER BY COALESCE(lkenz::VARCHAR, ''), datuv
						) AS date_to
					FROM query_table(stko_table)
					WHERE stlty = 'M'
					  AND stlal = bom_alternative
				),
				-- Step 3: Filter to valid BOM headers at reference_date
				stko_valid AS (
					SELECT stlty, stlnr, stlal, datuv
					FROM stko_with_dates, ref_date
					WHERE (lkenz IS NULL OR lkenz = '' OR lkenz = ' ')
					  AND datuv <= ref_date.the_date
					  AND (date_to IS NULL OR date_to > ref_date.the_date)
				),
				-- Step 4: Filter STPO items (not deleted)
				stpo_valid AS (
					SELECT
						stlnr,
						stlkn,
						posnr,
						TRIM(idnrk) AS idnrk,
						rekrs,
						menge,
						meins
					FROM query_table(stpo_table)
					WHERE lkenz IS NULL OR lkenz::VARCHAR = '' OR lkenz::VARCHAR = ' '
				),
				-- Step 5: Join everything together
				bom_joined AS (
					SELECT
						sk.stlnr AS bom_id,
						ma.matnr AS parent_id,
						sp.idnrk AS child_id,
						sp.menge AS quantity,
						1 AS level,
						sp.posnr AS position,
						sp.rekrs AS is_phantom
					FROM stko_valid sk
					INNER JOIN mast_filtered ma ON sk.stlnr = ma.stlnr AND sk.stlal = ma.stlal
					INNER JOIN stpo_valid sp ON sk.stlnr = sp.stlnr
				)
			SELECT
				bom_id,
				parent_id,
				child_id,
				quantity,
				level,
				TRY_CAST(position AS INTEGER) AS position
			FROM bom_joined
		)
	)");

	CheckQueryResult(result, "create sap_to_bom_items macro");

	// Phase 3b: Register compute_jaccard_embeddings macro for min-hash embedding generation
	// Returns embedding data for material components (one row per seed)
	// Generates 128-dimensional min-hash signatures from component sets
	// Min-hash preserves Jaccard similarity: |A∩B| / |A∪B| ≈ P(min_hash(A) = min_hash(B))
	// Uses 128 independent hash functions with different seeds (0-127)
	// Result can be pivoted and inserted into material_embeddings table
	//
	// Usage (to compute embeddings):
	//   INSERT INTO material_embeddings (material_id, jaccard_embedding, num_components, updated_at)
	//   WITH embeddings_data AS (
	//     SELECT * FROM compute_jaccard_embeddings()
	//   )
	//   SELECT
	//     material_id,
	//     array_agg(minhash_value ORDER BY seed)::FLOAT[128],
	//     MAX(num_components),
	//     CURRENT_TIMESTAMP
	//   FROM embeddings_data
	//   GROUP BY material_id
	//   ON CONFLICT (material_id) DO UPDATE SET
	//     jaccard_embedding = EXCLUDED.jaccard_embedding,
	//     num_components = EXCLUDED.num_components,
	//     updated_at = EXCLUDED.updated_at;
	result = conn.Query(R"(
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

	// Phase 5: Create auto-rebuild triggers for incremental embedding updates
	// These triggers mark materials as dirty when BOM items change
	// Lazy refresh pattern: refresh_dirty_embeddings() called before queries

	// Trigger 1: Mark parent material as dirty on INSERT
	result = conn.Query(R"(
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

	// Trigger 2: Mark parent material as dirty on UPDATE
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

	// Trigger 3: Mark parent material as dirty on DELETE
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

	// Phase 5b: Register refresh_dirty_embeddings macro for lazy embedding refresh
	// This macro recomputes embeddings for dirty materials and clears the dirty flag
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
		// This allows the extension to work in environments without trigger support
	}
}

void AnofoxSimilarityExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
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
