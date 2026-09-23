#include "modules/wl_kernel.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"
#include "datazoo_function_doc.hpp"

namespace ddoc = datazoo::doc;

namespace duckdb {
namespace anofox {

static const DefaultTableMacro BOM_DFS_NEIGHBORHOOD_MACRO = {
    DEFAULT_SCHEMA,
    "bom_dfs_neighborhood",
    {nullptr},
    {{"root_material_id", "''"}, {"max_depth", "3"}, {"bom_table", "'bom_items'"}, {nullptr, nullptr}},
    R"(
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
		SELECT DISTINCT component FROM dfs)"};

static const DefaultTableMacro WL_FINGERPRINT_MACRO = {
    DEFAULT_SCHEMA,
    "wl_fingerprint",
    {"root", "iters", "bom_table", nullptr},
    {{nullptr, nullptr}},
    R"(
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
		GROUP BY component)"};

static const DefaultMacro WL_KERNEL_SIMILARITY_MACRO = {
    DEFAULT_SCHEMA,
    "wl_kernel_similarity",
    {"material_a", "material_b", nullptr},
    {{"iterations", "3"}, {"bom_table", "'bom_items'"}, {nullptr, nullptr}},
    R"( (
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
		))"};

void RegisterWLKernelMacros(ExtensionLoader &loader) {
	ddoc::Registrar reg(loader, {"similarity", "bom"});
	reg.RegisterTableMacro(BOM_DFS_NEIGHBORHOOD_MACRO,
	           {ddoc::Doc()
	                .Describe("All descendant components of a root material within a depth limit, as a reusable depth-first BOM traversal over a (parent_id, child_id) edge table.")
	                .Example("SELECT * FROM bom_dfs_neighborhood(root_material_id := 'PUMP-A', max_depth := 3)")});
	reg.RegisterTableMacro(WL_FINGERPRINT_MACRO,
	           {ddoc::Doc()
	                .Describe("Depth-expanded component fingerprint of one material, as component -> number of distinct depths at which it is reachable. Written as a non-recursive bounded expansion on purpose: DuckDB cannot decorrelate a recursive CTE inside a correlated scalar subquery, which is exactly how wl_kernel_similarity uses it. Iterations 1-5 are exact; higher values are capped at depth 5.")
	                .Example("SELECT * FROM wl_fingerprint('PUMP-A', 3, 'bom_items')")});
	reg.RegisterMacro(WL_KERNEL_SIMILARITY_MACRO,
	           {ddoc::Doc()
	                .Describe("Weisfeiler-Lehman kernel similarity between two materials' BOM structures, comparing their depth-expanded component fingerprints. Returns a score in [0, 1] where 1 means structurally identical. Iterations 1-5 are exact; higher values are capped at depth 5.")
	                .Example("SELECT wl_kernel_similarity('PUMP-A', 'PUMP-B')")});
}

} // namespace anofox
} // namespace duckdb
