#include "modules/wl_kernel.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

void RegisterWLKernelMacros(Connection &conn) {
	// Helper macro: bom_dfs_neighborhood - Reusable BOM depth-first traversal
	// Extracts all descendant components within specified depth limit
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO bom_dfs_neighborhood(
			root_material_id := '',
			max_depth := 3,
			bom_table := 'bom_items'
		) AS TABLE
		WITH RECURSIVE dfs(component, depth) AS (
			-- Base case: direct children
			SELECT DISTINCT child_id AS component, 0 AS depth
			FROM query_table(bom_table)
			WHERE parent_id = root_material_id

			UNION ALL

			-- Recursive case: children of children
			SELECT DISTINCT qb.child_id AS component, dfs.depth + 1
			FROM dfs
			INNER JOIN query_table(bom_table) qb ON dfs.component = qb.parent_id
			WHERE dfs.depth < max_depth
		)
		SELECT DISTINCT component FROM dfs
	)");
	CheckQueryResult(result, "create bom_dfs_neighborhood helper macro");

	// wl_kernel_similarity: Weisfeiler-Lehman kernel for graph-structural similarity
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO wl_kernel_similarity(
			material_a,
			material_b,
			iterations := 3,
			bom_table := 'bom_items'
		) AS (
			WITH RECURSIVE
				-- Phase 1: Extract neighborhood around each material using DFS helper
				neighborhood_a AS (
					SELECT component FROM bom_dfs_neighborhood(material_a, iterations, bom_table)
				),
				neighborhood_b AS (
					SELECT component FROM bom_dfs_neighborhood(material_b, iterations, bom_table)
				),
				-- Phase 2: Count component occurrences per material (structured histogram)
				-- Using explicit DFS with depth tracking for fingerprints
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
