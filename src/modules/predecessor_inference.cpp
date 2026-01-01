#include "modules/predecessor_inference.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

void RegisterPredecessorInferenceMacros(Connection &conn) {
	// infer_predecessors: Historical consumption-aware predecessor detection
	auto result = conn.Query(R"(
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
}

} // namespace anofox
} // namespace duckdb
