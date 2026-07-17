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
		-- UNION (not UNION ALL) makes this set-based: the recursive engine only feeds NEW
		-- (component, depth) rows forward, so multiple paths that reach a component at the same depth
		-- collapse to one — no exponential blow-up on diamond BOMs. The depth gate is clamped to a
		-- hard ceiling so a cyclic BOM with a huge max_depth cannot iterate unbounded and hang.
		WITH RECURSIVE dfs(component, depth) AS (
			-- Base case is depth=1 (direct children = 1 hop), matching bom_explosion_multilevel's
			-- convention exactly, so max_depth means the same thing ("N hops") in both macros. The
			-- previous depth=0 base case made max_depth := N return N+1 hops (an off-by-one).
			SELECT DISTINCT child_id AS component, 1 AS depth
			FROM query_table(bom_table)
			WHERE parent_id = root_material_id

			UNION

			SELECT qb.child_id AS component, dfs.depth + 1
			FROM dfs
			INNER JOIN query_table(bom_table) qb ON dfs.component = qb.parent_id
			-- COALESCE handles max_depth := NULL; LEAST(...,64) bounds cyclic traversal.
			WHERE dfs.depth < LEAST(COALESCE(max_depth, 3), 64)
		)
		SELECT DISTINCT component FROM dfs
	)");
	CheckQueryResult(result, "create bom_dfs_neighborhood helper macro");

	// Helper: depth-expanded component fingerprint for one material, as component -> #distinct depths.
	// This is written as a NON-recursive, bounded-depth (0..4) expansion on purpose. DuckDB cannot
	// decorrelate a recursive CTE that lives inside a correlated scalar subquery, which is exactly how
	// wl_kernel_similarity is used ("fixed query material vs a candidate COLUMN"); the recursive form
	// threw an INTERNAL Error at any real scale. Each depth level is gated by `level < iterations`, so
	// iterations 1..5 are exact; iterations > 5 are capped at depth 5 (documented). Depth 5 already
	// captures the structure the WL kernel needs in practice.
	//
	// Each level is a SEPARATE CTE that DISTINCTs the reachable-component set BEFORE joining to the
	// next level (rather than one long e0..e4 join chain). COUNT(DISTINCT depth) only needs the SET
	// of components reachable at each depth, not how many paths reach them, so this is exactly
	// equivalent — but it is O(depth * edges) instead of O(fanout^depth): on a dense/complete BOM the
	// undeduped join chain reproduced every path (up to out-degree^4 rows at level 4), which hung.
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO wl_fingerprint(root, iters, bom_table) AS TABLE
		WITH
			level0 AS (
				SELECT DISTINCT child_id AS component, 0 AS depth
				FROM query_table(bom_table)
				WHERE parent_id = root AND 0 < COALESCE(iters, 3)
			),
			level1 AS (
				SELECT DISTINCT e.child_id AS component, 1 AS depth
				FROM level0 l JOIN query_table(bom_table) e ON l.component = e.parent_id
				WHERE 1 < COALESCE(iters, 3)
			),
			level2 AS (
				SELECT DISTINCT e.child_id AS component, 2 AS depth
				FROM level1 l JOIN query_table(bom_table) e ON l.component = e.parent_id
				WHERE 2 < COALESCE(iters, 3)
			),
			level3 AS (
				SELECT DISTINCT e.child_id AS component, 3 AS depth
				FROM level2 l JOIN query_table(bom_table) e ON l.component = e.parent_id
				WHERE 3 < COALESCE(iters, 3)
			),
			level4 AS (
				SELECT DISTINCT e.child_id AS component, 4 AS depth
				FROM level3 l JOIN query_table(bom_table) e ON l.component = e.parent_id
				WHERE 4 < COALESCE(iters, 3)
			)
		SELECT component, COUNT(DISTINCT depth) AS occurrence
		FROM (
			SELECT * FROM level0 UNION ALL SELECT * FROM level1 UNION ALL SELECT * FROM level2
			UNION ALL SELECT * FROM level3 UNION ALL SELECT * FROM level4
		)
		GROUP BY component
	)");
	CheckQueryResult(result, "create wl_fingerprint helper macro");

	// wl_kernel_similarity: Weisfeiler-Lehman kernel for graph-structural similarity.
	// Similarity is the bounded weighted Jaccard sum(min)/sum(max) over the union of the two
	// materials' depth-expanded fingerprints; it lies in [0, 1] and equals 1 for a material vs itself.
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO wl_kernel_similarity(
			material_a,
			material_b,
			iterations := 3,
			bom_table := 'bom_items'
		) AS (
			WITH
				fingerprint_a AS (SELECT * FROM wl_fingerprint(material_a, GREATEST(COALESCE(iterations, 3), 1), bom_table)),
				fingerprint_b AS (SELECT * FROM wl_fingerprint(material_b, GREATEST(COALESCE(iterations, 3), 1), bom_table)),
				totals AS (
					SELECT
						COALESCE((SELECT SUM(occurrence) FROM fingerprint_a), 0)::DOUBLE AS total_a,
						COALESCE((SELECT SUM(occurrence) FROM fingerprint_b), 0)::DOUBLE AS total_b,
						COALESCE((
							SELECT SUM(LEAST(fa.occurrence, fb.occurrence))
							FROM fingerprint_a fa
							INNER JOIN fingerprint_b fb ON fa.component = fb.component
						), 0)::DOUBLE AS total_intersection
				)
			SELECT CASE
				WHEN material_a = material_b THEN 1.0
				WHEN (SELECT total_a + total_b - total_intersection FROM totals) = 0 THEN 0.0
				ELSE (SELECT total_intersection FROM totals)
				     / (SELECT total_a + total_b - total_intersection FROM totals)
			END
		)
	)");

	CheckQueryResult(result, "create wl_kernel_similarity macro");
}

} // namespace anofox
} // namespace duckdb
