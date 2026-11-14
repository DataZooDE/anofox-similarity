# Upgrading

> Changes that need action when you move to a newer release. Newest first.

[API index](API_REFERENCE.md) · [Conventions](api/00-conventions.md)

## Embeddings as FLOAT arrays, typed universal-schema converters

Released with the Claude Code plugin (the first release after v2026.10.23).

### Recompute stored Jaccard embeddings

`compute_jaccard_embeddings` now scales each MinHash value to [0, 1) (it stored the raw 64-bit hash
cast to FLOAT, about 1e18). Old and new values are both `FLOAT[128]`, so mixing them is not an
error, but `minhash_similarity(old, new)` is 0.0 even for identical component sets. Recompute
every stored vector:

```sql
UPDATE material_embeddings SET jaccard_embedding = NULL;
-- then re-run your upsert from compute_jaccard_embeddings (docs/api/04-embeddings.md)
```

### Embedding types

| Function | Before | Now |
|---|---|---|
| `embed_text`, `embedding_backend`, `compute_textual_embeddings` | `FLOAT[]` | `FLOAT[384]` |
| `fuse_embeddings` arguments | `FLOAT[]`, `FLOAT[]`, `FLOAT[]` | `FLOAT[256]`, `FLOAT[384]`, `FLOAT[128]` (lists of these lengths cast in) |
| `fuse_embeddings`, `compute_fused_embeddings` result | `FLOAT[]` | `FLOAT[768]` |
| `compute_jaccard_embeddings.minhash_value` | raw hash as FLOAT | FLOAT in [0, 1) |
| `compute_jaccard_embeddings.seed` | INTEGER | BIGINT |

The array types are what DuckDB `vss` indexes and what `array_cosine_distance` and its siblings
take; `list_cosine_similarity` and other list functions still accept them. Two behaviour changes:

- A modality vector of the wrong length is now a cast error for the whole query
  ("Cannot cast list with length 2 to array with length 256"), not a NULL row. Filter or fix such
  rows first (`WHERE len(structural_embedding) = 256`).
- On DuckDB 1.5.6 a `CASE` over an array fails ("Unimplemented type for case expression"). Cast
  inside and back: `(CASE WHEN c THEN embed_text(t)::FLOAT[] END)::FLOAT[384]`.

### Other changes

| Was | Now |
|---|---|
| `items_to_bom(items, source)` with a `table_type` column | `items_to_materials(items, source)`, `items_to_bom_header(items, source)`, `items_to_bom_component(items)`, each in the universal table's columns and types |
| `items_to_bom` header `valid_from` 2024-01-01, materials `description` '' | NULL |
| `embedding_backend(..., '<any name>', ...)` returned the placeholder | only `gemma-local`, `openai`, `anthropic`, `cohere`; any other name is an error |
| `tree_class_multiset(..., attrs_table := ...)` (ignored) | the parameter is gone: drop it |
| `cosine_floor` parameters named `table`, `column` | `table_name`, `column_name` (still positional) |
| `check_statistics_freshness` on empty statistics: `last_updated` = now − 8 days | NULL |
| `dynamics365_to_bom_header`: one row per BOMVersion | one row per BOM (approved version, then the newest) |

Migrating from `items_to_bom`:

```sql
-- before: SELECT * FROM items_to_bom('bom_items', 'MIGRATED') WHERE table_type = 'bom_header'
CREATE TABLE bom_header AS SELECT * FROM items_to_bom_header('bom_items', 'MIGRATED');
CREATE TABLE bom_component AS SELECT * FROM items_to_bom_component('bom_items');
CREATE TABLE materials AS SELECT * FROM items_to_materials('bom_items', 'MIGRATED');
```
