# Material Similarity Examples

> **Two materials are alike when they are built from the same parts.**

Runnable SQL examples for comparing bills of materials (BOMs) with the anofox-similarity
extension: set overlap (Jaccard), structural similarity over several BOM levels
(Weisfeiler-Lehman kernel) and BOM traversal.

## Functions

| Function | Description |
|----------|-------------|
| `jaccard_similarity(list_a, list_b)` | Set overlap of two lists: \|A ∩ B\| / \|A ∪ B\| |
| `find_similar_materials_jaccard(material_id, k, ...)` | Top-k materials by component overlap; `mode := 'containment'` for directional overlap |
| `wl_kernel_similarity(material_a, material_b, iterations, bom_table)` | Structural similarity over up to 5 BOM levels |
| `find_similar_materials_wl_kernel(material_id, k, ...)` | Top-k materials by WL kernel similarity |
| `bom_explosion_1level(query_material_id := ...)` | Direct children of a material |
| `bom_explosion_multilevel(query_material_id := ..., max_depth := 10)` | All levels below a material |
| `bom_where_used(child_material_id := ...)` | Direct parents of a component |
| `bom_common_components(material_1 := ..., material_2 := ...)` | Components two materials share |

## Example Files

| File | Description | Data Source |
|------|-------------|-------------|
| [`jaccard_similarity.sql`](jaccard_similarity.sql) | Scalar Jaccard, top-k search, containment mode | Inline + AdventureWorks |
| [`wl_kernel_similarity.sql`](wl_kernel_similarity.sql) | Structure vs. parts list, `iterations`, top-k search | Inline + AdventureWorks |
| [`bom_traversal.sql`](bom_traversal.sql) | Explosion, where-used, common components | AdventureWorks (reshaped) |

AdventureWorks is in `test/data/adventureworks/` (bicycle BOMs, 238 parents, 2 576 edges).

## Quick Start

```bash
# From the repository root
./build/release/duckdb < examples/material_similarity/jaccard_similarity.sql
./build/release/duckdb < examples/material_similarity/wl_kernel_similarity.sql
./build/release/duckdb < examples/material_similarity/bom_traversal.sql
```

---

## Usage

```sql
-- Top 5 materials sharing the most components with AW-749
SELECT * FROM find_similar_materials_jaccard('AW-749', 5);

-- Same, on another table, only candidates that contain at least 80 % of AW-749's parts
SELECT * FROM find_similar_materials_jaccard('AW-749', 5, bom_table := 'my_bom',
                                             mode := 'containment', min_similarity := 0.8);

-- Structure-aware: compare sub-assemblies too
SELECT * FROM find_similar_materials_wl_kernel('AW-996', 5, iterations := 3);
```

## Input Tables

| Table | Columns | Used by |
|-------|---------|---------|
| `bom_items` (default name) | `parent_id, child_id` (more columns are ignored) | Jaccard and WL functions |
| `bom_header` | `bom_id, source_system, parent_material_id` | BOM traversal macros |
| `bom_component` | `bom_id, child_material_id, quantity_per` | BOM traversal macros |

`bom_explosion_*`, `bom_where_used` and `bom_common_components` read the two-table shape that
the SAP and Dynamics 365 transforms emit. `bom_traversal.sql` shows how to build it from a flat
edge list.

## Key Concepts

- **Positional arguments.** The material id and `k` of the search functions are positional
  (`find_similar_materials_jaccard('AW-749', 5)`, not `k := 5`). The traversal macros take only
  named arguments (`query_material_id := 'AW-996'`).
- **Jaccard ignores quantities and depth.** Two BOMs with the same direct children score 1.0.
  For quantities, use `find_covering_entities` with `weight_col` (see
  [coverage_matching](../coverage_matching/)).
- **WL kernel looks deeper.** Two products whose direct children differ but whose
  sub-assemblies contain the same parts score high. `iterations` (1..5) sets the depth.
- **Zero scores are returned.** The search functions return candidates with similarity 0;
  filter with `min_similarity`.
