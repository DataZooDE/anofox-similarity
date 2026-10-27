# Conventions

> Naming, parameters, errors and performance notes that apply to every function.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

## Overview

The anofox-similarity extension provides about 87 functions across 9 functional categories for multi-modal product similarity detection and coverage matching. All functions are registered as SQL macros, table functions or scalar functions and follow consistent naming conventions.

### Naming Convention

- **Table Macros** (prefix `compute_`, `find_`, `infer_`) - Return result sets
- **Scalar Functions** (prefix `jaccard_`, `wl_kernel_`, `embed_`) - Return single values
- **Coverage matching** - set measures are named after the measure (`containment`, `tversky_similarity`, `weighted_*`); searches start with `find_`
- **Helper Macros** (prefix `check_`, `create_`, `bom_`) - Infrastructure utilities

### Parameterization Pattern

On DuckDB 1.5.6, named arguments of scalar functions are matched by position, so give them in the documented order; DuckDB 2.0 matches them by name.

All macros support default parameters with `:=` syntax:

```sql
SELECT * FROM macro_name(
  param1 := value1,    -- Optional, uses default if omitted
  param2 := value2
)
```

Arguments of table functions must be constants: DuckDB rejects a subquery there ("Table
function cannot contain subqueries"), and table parameters take a table name, not a query.
Compute a value first and pass it with a variable, or materialise the rows as a table:

```sql
SET VARIABLE floor = (SELECT floor FROM cosine_floor('name_vectors', 'vec'));
SELECT * FROM tree_coverage('Q1', k := 5, name_floor := getvariable('floor'));
```

---

## Parameter Reference

### Common Parameters

**Table References:**
- `bom_table` - Default: 'bom_items' (columns: parent_id, child_id)
- `movements_table` - Default: 'goods_movements' (columns: material_id, movement_date, quantity)

**Dimension Parameters:**
- `k` - Number of results to return: a required positional argument of the search functions
- `max_depth` - Maximum recursion depth (default: 10)
- `iterations` - Algorithm iterations (default: 3)

**Temporal Parameters:**
- `time_window_days` - Historical window (default: 365)
- `lookback_months` - Lookback period (default: 24)
- `lag_weeks` - Expected temporal lag (default: 8)

**Quality Parameters:**
- `min_observations` - Minimum data points (default: 3)
- `min_similarity` - Minimum similarity threshold (default: 0.0)
- `min_overlapping_weeks` - Minimum aligned weekly buckets for predecessor inference (default: 8)

**Weighting Parameters:**
- `weights_structural` - Structural embedding weight (default: 0.5)
- `weights_textual` - Textual embedding weight (default: 0.5)
- `weights_transactional` - Transactional embedding weight (default: 0.0)

---

## Error Handling

All functions implement graceful error handling:

| Scenario | Behavior |
|----------|----------|
| Material not found | Returns empty result set (no error) |
| Insufficient data (< min_observations) | Filters out via HAVING clause |
| Circular BOM reference | Handled via depth limit |
| NULL embedding | Treated as missing, skipped in fusion |
| VSS extension unavailable | Falls back to brute-force k-NN |
| anofox-forecast unavailable | `compute_transactional_embeddings` raises a clear "install anofox-forecast" error; the SQL-macro functions (`extract_ts_features`, `recompute_embedding_statistics`, `compute_domain_specific_statistics`) fail at bind time with a catalog error naming `anofox_fcst_ts_features` — all require the extension in full (no partial features) |
| NULL k / iterations on a search function | Clean binder error (not an internal crash) |
| Weights not summing to 1 in fusion | Normalized automatically |

---

## Performance Notes

### Complexity Summary

| Function | Best Case | Worst Case | Notes |
|----------|-----------|-----------|-------|
| Jaccard similarity | O(n) | O(n) | Linear in component count |
| WL Kernel | O(h·m) | O(h·n²) | h=iterations, m=edges |
| Find similar (indexed) | O(log n) | O(log n + k) | With HNSW index |
| Find similar (brute) | O(n·m) | O(n²) | Without index |
| BOM explosion | O(c) | O(depth × edges) | c=children; set-based traversal, not exponential (safe on cycles/diamonds) |
| Textual embeddings | O(n) | O(n) | Linear in material count |

### Memory Considerations

- **HNSW index**: ~100 bytes per node (10K materials ≈ 1MB)
- **Embeddings**: 512 bytes per material (128D × 4B float)
- **Statistics table**: 100-200 KB for 104 features

---

## Function Names

All functions are registered in the `main` schema under the names shown in this document; call them
unqualified:
```sql
SELECT * FROM find_similar_materials_jaccard('MAT-001', 10);
```
There is no `anofox_similarity.<function>` schema-qualified form and no `anofox_`-prefixed alias.

---
