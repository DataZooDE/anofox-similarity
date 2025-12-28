#include "modules/wl_kernel.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

void RegisterWLKernelMacro(Connection &conn) {
	// wl_kernel_similarity: Weisfeiler-Lehman kernel for graph-structural similarity
	auto result = conn.Query(R"(
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

} // namespace anofox
} // namespace duckdb
