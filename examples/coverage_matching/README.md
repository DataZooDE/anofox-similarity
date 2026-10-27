# Coverage Matching Examples

> **Similarity asks "how alike are A and B?". Coverage asks "does B give what A asks for?".**

Runnable SQL examples for matching a request (a customer specification) against offers (past
programmes, modules). Stage 1 ranks candidates by their class multisets; stage 2 checks every
required attribute value with its direction and every count, and lists the gaps.

## Functions

| Function | Description |
|----------|-------------|
| `find_covering_entities(entity_id, k, ...)` | Stage 1: rank entities of an `(entity, feature, weight)` table by how much of the query they cover |
| `find_covering_entities_for(map, k, ...)` | The same for a query given as `MAP(feature, weight)` |
| `containment`, `weighted_containment`, `weighted_jaccard`, `weighted_tversky` | The directional scalars behind stage 1 |
| `tree_coverage(query_tree_id \| list, ...)` | Stage 1 + 2 on property trees: coverage, rank, number of gaps |
| `tree_coverage_trace(query_tree_id, ...)` | The same score explained per requirement and attribute (the gap list) |

## Example Files

| File | Description | Data Source |
|------|-------------|-------------|
| [`find_covering_entities.sql`](find_covering_entities.sql) | Containment vs. Jaccard, MAP queries, full covers only | `test/data/coverage/programme_class_multiset.csv.gz` |
| [`tree_coverage_mini.sql`](tree_coverage_mini.sql) | Ranking, gap list, stage-1 candidates, OR variants | `test/data/coverage/mini/` (hand-made) |
| [`tree_coverage_benchmark.sql`](tree_coverage_benchmark.sql) | Full pipeline: parse values, rank 300 programmes, gaps of an unmeetable request, 80 requests at once | `test/data/coverage/` (seating benchmark) |

The benchmark is described in [`test/data/coverage/README.md`](../../test/data/coverage/README.md).
Files starting with `gold_` are ground truth. The examples use `gold_item_class` and
`gold_vocabulary` only as stand-ins for a finished class review (see
[classification_review](../classification_review/) for the classifier path), and
`gold_coverage` / `gold_requests` only to score results.

## Quick Start

```bash
# From the repository root
./build/release/duckdb < examples/coverage_matching/find_covering_entities.sql
./build/release/duckdb < examples/coverage_matching/tree_coverage_mini.sql
./build/release/duckdb < examples/coverage_matching/tree_coverage_benchmark.sql
```

---

## Usage

```sql
-- Stage 1 only: which programmes contain 2 heated zones, 4 massage zones, ...?
SELECT entity_id, score, containment, jaccard
FROM find_covering_entities_for(MAP {'seat_heating': 2, 'massage': 4}, 10,
                                feature_table := 'programme_classes');

-- Stage 1 + 2 on trees, then the gap list of the best candidate
SELECT * FROM tree_coverage('Q1', k := 20);
SELECT * FROM tree_coverage_trace('Q1', candidates := ['S1']) WHERE is_gap;
```

## Input Tables

| Table | Columns |
|-------|---------|
| feature table (`feature_table`) | `entity_id, feature_id, weight`; remap with `entity_col`, `feature_col`, `weight_col` |
| tree nodes (`nodes_table`, default `tree_nodes`) | `tree_id, node_id, parent_id, node_kind, edge_type, label, class_id, count[, source, context]` |
| tree attributes (`attrs_table`, default `tree_attributes`) | `tree_id, node_id, attr_id, name, dimension, unit, lo, hi[, bound, category]`, values in base units |
| `overrides_table` | reviewed vocabulary `wording, source, class_id` |
| `templates_table` | catalogue attribute templates (give unqualified values a bound) |
| `rules_table` | `dimension, required, provided, score` for ordinal classes |

## Key Concepts

- **Counts matter.** 4 massage zones are not covered by 2: weights are compared with Σmin.
- **Lean covers first.** The default score is containment + 0.01 × Jaccard, so among full
  covers the one with the least surplus wins. `min_score := 1.0` keeps full covers only.
- **Stage 2 overrules stage 1.** A programme with the right classes but failing values
  (S2 in the mini fixture) drops in the ranking.
- **One query list, many requests.** `tree_coverage` takes a `VARCHAR[]` of request ids and
  ranks all of them in one call.
- **The trace explains the score.** Pass the same parameters to `tree_coverage_trace`; the mean
  of its requirement scores is the coverage.
