# Conventions

> Naming, parameters, errors and performance notes that apply to every function.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

## Overview

The extension registers 79 functions: scalar functions, table functions and SQL macros. The
[API index](../API_REFERENCE.md) lists them by topic; each topic page ends with a generated
reference (kind, signature, description, examples).

### Naming Convention

- **Searches** start with `find_` (`find_similar_materials_jaccard`, `find_covering_entities`)
- **Builders** start with `compute_` (embeddings) or end in the thing they build (`minhash_index`, `tree_from_json`)
- **Set measures** are named after the measure (`jaccard_similarity`, `containment`, `tversky_similarity`, `weighted_*`)
- **Helpers** start with `check_`, `create_` or `bom_`

### Glossary

| Term | Meaning |
|---|---|
| query / request | what you search for: a material, a feature multiset, or a request tree |
| candidate / entity / offer / programme | what is searched: BOMs, feature multisets, or candidate trees |
| feature multiset | rows `(entity_id, feature_id, weight)`; without a weight column every feature counts once |
| class | a catalogue category an item of a tree is mapped to; coverage compares classes, then values |
| class resolution | an item's class comes from `class_id`, else the reviewed vocabulary (`overrides_table`), else the classifier (`label_table`) |
| reviewed | a tree whose item classes all come from `class_id` or the reviewed vocabulary |

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

Bad input raises an error that names the function and the parameter; data that simply does not
match gives an empty result.

| Scenario | Behavior |
|----------|----------|
| Unknown material id in a material search | Empty result (no error) |
| Unknown query tree, or a request without classified items, in `tree_coverage` | Error naming the tree and the missing input |
| Table parameter names a table that does not exist | Error (catalog or binder error naming the table) |
| Table lacks a required column | Error naming the table, the column and the parameter |
| Identifier that is not a plain or qualified name (`a;drop`) | Error ("Invalid identifier for ...") |
| NULL `k` or another required argument | Error ("k must not be NULL") |
| Negative or non-finite weights | Error naming the entity and feature |
| NULL list in a set measure | NULL; an empty request gives NULL for directional measures, and `jaccard_similarity([], [])` is 0.0 |
| Different dimensions in `value_satisfies` (5 A vs 5 V) | Not an error: score 0.0 with rule `gate` |
| Circular BOM reference | Traversal stops at the depth limit |
| anofox-forecast not loaded | `compute_transactional_embeddings` raises an "install anofox-forecast" error; the SQL macros that need it (`extract_ts_features`, `recompute_embedding_statistics`, `compute_domain_specific_statistics`) fail with a catalog error naming `anofox_fcst_ts_features` |

---

## Performance Notes

| Function | Cost | Notes |
|----------|------|-------|
| Set measures (`jaccard_similarity`, `containment`, ...) | linear in the list lengths | |
| `find_similar_materials_jaccard` | one comparison per BOM | exact; no index is used |
| `find_similar_materials_wl_kernel` | one recursive pass over the BOM table | depth capped at 5 levels |
| BOM explosion | linear in the edges per level | set-based traversal, safe on cycles and diamonds |
| `find_covering_entities` | one pass over the candidates sharing a feature with the query | inverted join on `feature_id` |
| `find_covering_entities_lsh` | only the candidates sharing a band | approximate; see [LSH](16-lsh-and-clustering.md) |
| `tree_coverage` | stage 2 runs on the top `k` stage-1 candidates only | |

Measured timings for the coverage functions are on the [coverage kernel](13-coverage-kernel.md)
page, with the test that reproduces them.

---

## Function Names

All functions are registered in the `main` schema under the names shown in this document; call them
unqualified:
```sql
SELECT * FROM find_similar_materials_jaccard('MAT-001', 10);
```
There is no `anofox_similarity.<function>` schema-qualified form and no `anofox_`-prefixed alias.

---
